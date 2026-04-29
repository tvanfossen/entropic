// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file tool_executor.cpp
 * @brief ToolExecutor implementation.
 * @version 2.0.6-rc16
 */

#include <entropic/mcp/tool_executor.h>
#include <entropic/mcp/tool_result_classify.h>
#include <entropic/mcp/utf8_sanitize.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>

static auto logger = entropic::log::get("mcp.tool_executor");

namespace entropic {

/**
 * @brief Construct with shared dependencies.
 * @param server_manager Server manager.
 * @param loop_config Loop configuration.
 * @param callbacks Shared callbacks.
 * @param hooks Optional engine-level hooks.
 * @internal
 * @version 1.8.5
 */
ToolExecutor::ToolExecutor(
    ServerManager& server_manager,
    const LoopConfig& loop_config,
    EngineCallbacks& callbacks,
    ToolExecutorHooks hooks)
    : server_manager_(server_manager),
      loop_config_(loop_config),
      callbacks_(callbacks),
      hooks_(hooks) {}

/**
 * @brief Set permission persistence interface.
 * @param persist Permission persist callbacks.
 * @internal
 * @version 1.8.8
 */
void ToolExecutor::set_permission_persist(
    const PermissionPersistInterface& persist) {
    permission_persist_ = persist;
}

/**
 * @brief Process a batch of tool calls.
 * @param ctx Loop context (provides effective_max_tool_calls_per_turn, P3-18).
 * @param tool_calls Tool calls from model output.
 * @return Result messages.
 * @internal
 * @version 2.0.6-rc16
 */
std::vector<Message> ToolExecutor::process_tool_calls(
    LoopContext& ctx,
    const std::vector<ToolCall>& tool_calls) {
    logger->info("Processing {} tool calls", tool_calls.size());
    ctx.state = AgentState::WAITING_TOOL;
    fire_state_callback(ctx);

    auto limited = sort_tool_calls(tool_calls);
    int eff_limit = ctx.effective_max_tool_calls_per_turn >= 0
        ? ctx.effective_max_tool_calls_per_turn
        : loop_config_.max_tool_calls_per_turn;
    truncate_to_limit(limited, eff_limit);

    std::vector<Message> results;
    for (const auto& call : limited) {
        auto msgs = process_single_call(ctx, call);
        for (auto& m : msgs) {
            results.push_back(std::move(m));
        }
        if (should_stop_batch(ctx, results)) {
            break;
        }
    }

    ctx.consecutive_errors = 0;
    return results;
}

/**
 * @brief Sort tool calls so entropic.delegate is last.
 * @param calls Input tool calls.
 * @return Sorted copy.
 * @internal
 * @version 1.8.5
 */
std::vector<ToolCall> ToolExecutor::sort_tool_calls(
    const std::vector<ToolCall>& calls) {
    auto sorted = calls;
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const ToolCall& a, const ToolCall& b) {
            bool a_delegate = (a.name == "entropic.delegate");
            bool b_delegate = (b.name == "entropic.delegate");
            return !a_delegate && b_delegate;
        });
    return sorted;
}

/**
 * @brief Check for duplicate tool call.
 * @param ctx Loop context.
 * @param call Tool call.
 * @return Previous result or empty.
 * @internal
 * @version 1.8.5
 */
std::string ToolExecutor::check_duplicate(
    const LoopContext& ctx,
    const ToolCall& call) const {
    if (server_manager_.skip_duplicate_check(call.name)) {
        return "";
    }
    auto key = tool_call_key(call);
    auto it = ctx.recent_tool_calls.find(key);
    if (it != ctx.recent_tool_calls.end()) {
        return it->second;
    }
    return "";
}

/**
 * @brief Handle duplicate tool call.
 * @param ctx Loop context.
 * @param call Duplicate tool call.
 * @param previous_result Previous result.
 * @return Feedback message.
 * @internal
 * @version 1.8.5
 */
Message ToolExecutor::handle_duplicate(
    LoopContext& ctx,
    const ToolCall& call,
    const std::string& previous_result) {
    ctx.consecutive_duplicate_attempts++;
    logger->warn("Duplicate tool call #{}: {}",
                 ctx.consecutive_duplicate_attempts, call.name);

    if (ctx.consecutive_duplicate_attempts >= 3) {
        return create_circuit_breaker_message();
    }
    return create_duplicate_message(call, previous_result);
}

/**
 * @brief Check tool approval.
 * @param call Tool call.
 * @return true if approved.
 * @internal
 * @version 1.9.1
 */
bool ToolExecutor::check_approval(const ToolCall& call) {
    auto args_json = serialize_args(call);
    bool auto_ok = loop_config_.auto_approve_tools
                || server_manager_.is_explicitly_allowed(
                       call.name, args_json);

    bool approved = auto_ok;
    if (!approved && callbacks_.on_tool_call != nullptr) {
        auto call_json = serialize_tool_call(call);
        callbacks_.on_tool_call(call_json.c_str(),
                                callbacks_.user_data);
        approved = true;
    }

    // Hook: ON_PERMISSION_CHECK — informational (v1.9.1)
    if (hook_iface_.fire_info != nullptr) {
        std::string perm = approved ? "allowed" : "denied";
        std::string json = "{\"tool_name\":\""
            + call.name + "\",\"permission\":\"" + perm + "\"}";
        hook_iface_.fire_info(hook_iface_.registry,
            ENTROPIC_HOOK_ON_PERMISSION_CHECK, json.c_str());
    }

    if (!approved) {
        logger->warn("No approval callback — denying: {}", call.name);
    }
    return approved;
}

/**
 * @brief Check tier allowed_tools restrictions.
 * @param ctx Loop context.
 * @param call Tool call.
 * @return Rejection message or nullopt.
 * @internal
 * @version 1.8.5
 */
std::optional<Message> ToolExecutor::check_tier_allowed(
    const LoopContext& ctx, const ToolCall& call) const {
    // Tier allowlist enforcement is wired when the facade provides
    // tier config. For now, pass through (no tier locked = no filter).
    if (ctx.locked_tier.empty()) {
        return std::nullopt;
    }
    // Actual tier allowlist lookup deferred to facade integration
    return std::nullopt;
}

/**
 * @brief Check required fields are present.
 * @param schema Parsed JSON Schema object.
 * @param args Parsed tool arguments.
 * @return Error string, or empty on pass.
 * @utility
 * @version 2.0.6
 */
static std::string check_required_fields(
    const nlohmann::json& schema,
    const nlohmann::json& args)
{
    for (const auto& req : schema.value("required",
                                         nlohmann::json::array())) {
        if (!args.contains(req.get<std::string>())) {
            return "Missing required argument: "
                 + req.get<std::string>();
        }
    }
    return "";
}

/**
 * @brief Check one property's enum and type constraints.
 * @param key Property name.
 * @param prop Property schema.
 * @param val Argument value.
 * @return Error string, or empty on pass.
 * @utility
 * @version 2.0.6
 */
/**
 * @brief Check a single value against an enum constraint.
 * @param key Property name.
 * @param allowed Enum array from schema.
 * @param val Argument value.
 * @return Error string, or empty on pass.
 * @utility
 * @version 2.0.6
 */
static std::string check_enum(
    const std::string& key,
    const nlohmann::json& allowed,
    const nlohmann::json& val)
{
    for (const auto& e : allowed) {
        if (e == val) { return ""; }
    }
    return "Invalid value for '" + key + "': "
         + val.dump() + ". Must be one of: " + allowed.dump();
}

/**
 * @brief Check a single value against a type constraint.
 * @param key Property name.
 * @param type Expected JSON Schema type string.
 * @param val Argument value.
 * @return Error string, or empty on pass.
 * @utility
 * @version 2.0.6
 */
static std::string check_type(
    const std::string& key,
    const std::string& type,
    const nlohmann::json& val)
{
    bool ok = (type == "string" && val.is_string())
           || (type == "integer" && val.is_number_integer())
           || (type == "number" && val.is_number())
           || (type == "boolean" && val.is_boolean())
           || (type == "array" && val.is_array())
           || (type == "object" && val.is_object());
    return ok ? "" : "Type mismatch for '" + key
                   + "': expected " + type;
}

/**
 * @brief Check one property's enum and type constraints.
 * @param key Property name.
 * @param prop Property schema.
 * @param val Argument value.
 * @return Error string, or empty on pass.
 * @utility
 * @version 2.0.6
 */
static std::string check_property_constraints(
    const std::string& key,
    const nlohmann::json& prop,
    const nlohmann::json& val)
{
    if (prop.contains("enum")) {
        auto err = check_enum(key, prop["enum"], val);
        if (!err.empty()) { return err; }
    }
    if (!prop.contains("type")) { return ""; }
    return check_type(key, prop["type"].get<std::string>(), val);
}

/**
 * @brief Validate tool arguments against the tool's JSON Schema.
 *
 * Checks required fields, enum constraints, and basic type matching.
 * Returns an error string on violation, or empty on pass.
 *
 * @param schema_json The tool's input_schema (JSON Schema string).
 * @param args The parsed arguments from the model.
 * @return Error description, or empty string if valid.
 * @utility
 * @version 2.0.6
 */
static std::string validate_tool_args(
    const std::string& schema_json,
    const nlohmann::json& args)
{
    auto schema = nlohmann::json::parse(schema_json, nullptr, false);
    if (!schema.is_object()) { return ""; }

    auto err = check_required_fields(schema, args);
    auto props = schema.value("properties", nlohmann::json::object());
    for (auto it = props.begin(); it != props.end() && err.empty(); ++it) {
        if (args.contains(it.key())) {
            err = check_property_constraints(
                it.key(), it.value(), args[it.key()]);
        }
    }
    return err;
}

/**
 * @brief Execute a single tool call.
 * @param ctx Loop context.
 * @param call Tool call.
 * @return (result message, raw result string).
 * @internal
 * @version 2.1.1
 */
std::pair<Message, std::string> ToolExecutor::execute_tool(
    LoopContext& ctx, const ToolCall& call) {

    auto args_json = serialize_args(call);

    if (callbacks_.on_tool_start != nullptr) {
        auto call_json = serialize_tool_call(call);
        callbacks_.on_tool_start(call_json.c_str(),
                                 callbacks_.user_data);
    }

    auto start = std::chrono::steady_clock::now();
    // Inbound boundary from MCP server subprocess. v2.1.0 (#47) introduced
    // this; v2.1.1 (#3) generalized it as one of several boundary-policy
    // sanitize sites — see include/entropic/mcp/utf8_sanitize.h for the
    // full policy. The earlier "trust downstream" assumption was wrong:
    // bytes also enter via the model token stream and the audit-replay
    // path; both now sanitize at their own boundaries.
    auto result_json = mcp::sanitize_utf8(
        server_manager_.execute(call.name, args_json));
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(end - start).count();

    ctx.metrics.tool_calls++;

    // Parse result
    std::string result_text;
    try {
        auto j = nlohmann::json::parse(result_json);
        result_text = j.value("result", result_json);
    } catch (...) {
        result_text = result_json;
    }


    // P1-11 (2.0.6-rc16): stash into history ring buffer so the
    // constitutional validator revision prompt (and diagnostic tools)
    // can surface prior-iteration tool calls without re-reading
    // messages[].
    ToolCallRecord rec;
    rec.sequence = ++history_seq_;
    rec.tool_name = call.name;
    rec.params_summary = summarize_params(args_json);
    rec.status = (result_text.rfind("error", 0) == 0)
        ? "error" : "success";
    rec.result_summary = truncate_result(result_text, 200);
    rec.elapsed_ms = static_cast<double>(ms);
    rec.iteration = ctx.metrics.iterations;
    history_.record(rec);

    fire_tool_complete_callback(call, result_text, ms);

    Message msg;
    msg.role = "user";
    msg.content = result_text;
    msg.metadata["tool_call_id"] = call.id;
    msg.metadata["tool_name"] = call.name;

    return {std::move(msg), result_json};
}

/**
 * @brief Generate duplicate detection key.
 * @param call Tool call.
 * @return Key string.
 * @internal
 * @version 1.8.5
 */
std::string ToolExecutor::tool_call_key(const ToolCall& call) {
    // Sort arguments for consistent key
    nlohmann::json args;
    for (const auto& [k, v] : call.arguments) {
        args[k] = v;
    }
    return call.name + ":" + args.dump();
}

/**
 * @brief Record tool call for duplicate detection.
 * @param ctx Loop context.
 * @param call Tool call.
 * @param result Result string.
 * @internal
 * @version 1.8.5
 */
void ToolExecutor::record_tool_call(
    LoopContext& ctx,
    const ToolCall& call,
    const std::string& result) {
    // Extract result text from JSON envelope
    std::string text = result;
    try {
        auto j = nlohmann::json::parse(result);
        text = j.value("result", result);
    } catch (...) {}

    // Don't cache error results
    if (text.find("Error:") == 0 || text.find("error:") == 0) {
        return;
    }
    auto key = tool_call_key(call);
    ctx.recent_tool_calls[key] = text;
}

/**
 * @brief Create a permission denied message.
 * @param call Tool call.
 * @param reason Denial reason.
 * @return Feedback message.
 * @internal
 * @version 1.8.5
 */
Message ToolExecutor::create_denied_message(
    const ToolCall& call,
    const std::string& reason) {
    Message msg;
    msg.role = "user";
    msg.content =
        "Tool `" + call.name + "` was denied: " + reason + "\n\n"
        "This tool is not available to you. Do NOT retry it. "
        "Use a different approach to accomplish your task.";
    return msg;
}

/**
 * @brief Create a tool error message.
 * @param call Tool call.
 * @param error Error description.
 * @return Feedback message.
 * @internal
 * @version 1.8.5
 */
Message ToolExecutor::create_error_message(
    const ToolCall& call,
    const std::string& error) {
    Message msg;
    msg.role = "user";
    msg.content =
        "Tool `" + call.name + "` failed with error: " + error +
        "\n\nRECOVERY:\n"
        "- Check arguments are correct\n"
        "- Try a different approach\n"
        "- Do NOT retry with the same arguments";
    return msg;
}

// ── Private helpers ──────────────────────────────────────

/**
 * @brief Fire state change callback.
 * @param ctx Loop context.
 * @internal
 * @version 1.8.5
 */
void ToolExecutor::fire_state_callback(const LoopContext& ctx) {
    if (callbacks_.on_state_change != nullptr) {
        callbacks_.on_state_change(
            static_cast<int>(ctx.state), callbacks_.user_data);
    }
}

/**
 * @brief Truncate tool calls to the effective per-turn limit.
 * @param calls Tool call vector (mutated).
 * @param limit Effective limit (after per-identity override, P3-18).
 * @internal
 * @version 2.0.6-rc16
 */
void ToolExecutor::truncate_to_limit(
    std::vector<ToolCall>& calls,
    int limit) const {
    auto lim = static_cast<size_t>(limit);
    if (calls.size() > lim) {
        calls.resize(lim);
    }
}

/**
 * @brief Check MCP authorization for a tool call.
 * @param ctx Loop context (provides identity from locked_tier).
 * @param call Tool call.
 * @return Error message if denied, nullopt if authorized.
 * @internal
 * @version 1.9.4
 */
std::optional<Message> ToolExecutor::check_mcp_authorization(
    const LoopContext& ctx,
    const ToolCall& call) const {
    if (auth_mgr_ == nullptr) {
        return std::nullopt;
    }
    auto identity = ctx.locked_tier.empty()
                        ? "lead" : ctx.locked_tier;
    auto required = server_manager_.get_required_access_level(
        call.name);
    if (!auth_mgr_->is_enforced(identity) ||
        auth_mgr_->check_access(identity, call.name, required)) {
        return std::nullopt;
    }
    auto level_str = mcp_access_level_name(required);
    logger->warn("MCP key denied: {} requires {} for {}",
                 call.name, level_str, identity);
    Message msg;
    msg.role = "user";
    msg.content =
        "Tool `" + call.name + "` was denied: identity `"
        + identity + "` lacks " + level_str
        + " access.\n\n"
        "Your MCP key set does not authorize this tool. "
        "Use `entropic.delegate` to hand off to an identity "
        "that has the required access.";
    return msg;
}

/**
 * @brief Check duplicate detection and approval (layers within preconditions).
 * @param ctx Loop context.
 * @param call Tool call.
 * @return Rejection message if blocked, nullopt if clear.
 * @internal
 * @version 1.9.4
 */
std::optional<Message> ToolExecutor::check_dup_or_approval(
    LoopContext& ctx, const ToolCall& call) {
    auto dup_result = check_duplicate(ctx, call);
    if (!dup_result.empty()) {
        return handle_duplicate(ctx, call, dup_result);
    }
    ctx.consecutive_duplicate_attempts = 0;
    return check_approval(call)
        ? std::nullopt
        : std::optional{create_denied_message(
              call, "Permission denied")};
}

/**
 * @brief Check preconditions (MCP keys, duplicate, approval, tier).
 * @param ctx Loop context.
 * @param call Tool call.
 * @return Rejection message if blocked, nullopt if clear.
 * @internal
 * @version 1.9.4
 */
/**
 * @brief Validate tool arguments against schema constraints.
 * @param call Tool call to validate.
 * @return Rejection message, or nullopt on pass.
 * @internal
 * @version 2.0.6
 */
std::optional<Message> ToolExecutor::check_schema(
    const ToolCall& call) {
    auto schema = server_manager_.get_tool_schema(call.name);
    if (schema.empty()) { return std::nullopt; }
    auto args = nlohmann::json::parse(
        serialize_args(call), nullptr, false);
    auto err = args.is_discarded()
        ? std::string{} : validate_tool_args(schema, args);
    if (err.empty()) { return std::nullopt; }
    logger->warn("Tool '{}' argument validation failed: {}",
                 call.name, err);
    return create_denied_message(call, err);
}

/**
 * @brief Run all precondition checks for a tool call.
 * @param ctx Loop context.
 * @param call Tool call.
 * @return PreconditionCheck with rejection + typed kind on failure.
 * @internal
 * @version 2.0.6-rc19
 */
PreconditionCheck ToolExecutor::check_call_preconditions(
    LoopContext& ctx, const ToolCall& call) {
    PreconditionCheck pc;
    if (auto r = check_schema(call); r.has_value()) {
        pc.rejection = std::move(r);
        pc.kind = ToolResultKind::rejected_schema;
    } else if (auto a = check_mcp_authorization(ctx, call);
               a.has_value()) {
        pc.rejection = std::move(a);
        pc.kind = ToolResultKind::rejected_precondition;
    } else if (auto t = check_tier_allowed(ctx, call); t.has_value()) {
        pc.rejection = std::move(t);
        pc.kind = ToolResultKind::rejected_precondition;
    } else if (auto dup = check_duplicate(ctx, call); !dup.empty()) {
        pc.rejection = handle_duplicate(ctx, call, dup);
        pc.kind = ToolResultKind::rejected_duplicate;
    } else {
        pc = check_approval_pc(ctx, call);
    }
    return pc;
}

/**
 * @brief Terminal approval check extracted to keep the precondition
 *        chain under the nesting-depth gate.
 * @param ctx Loop context (mutated: duplicate counter reset).
 * @param call Tool call.
 * @return PreconditionCheck with rejection+kind set on denial, empty on pass.
 * @internal
 * @version 2.0.6-rc19
 */
PreconditionCheck ToolExecutor::check_approval_pc(
    LoopContext& ctx, const ToolCall& call) {
    PreconditionCheck pc;
    ctx.consecutive_duplicate_attempts = 0;
    if (!check_approval(call)) {
        pc.rejection = create_denied_message(
            call, "Permission denied");
        pc.kind = ToolResultKind::rejected_precondition;
    }
    return pc;
}

/**
 * @brief Process a single tool call (precondition check + execute).
 *
 * Emits one consolidated [tool_call] log entry after execution with
 * iter, tier, tool, elapsed_ms, result_chars, and status.
 *
 * @param ctx Loop context.
 * @param call Tool call.
 * @return Result messages (0 or 1).
 * @internal
 * @version 2.1.1
 */
std::vector<Message> ToolExecutor::process_single_call(
    LoopContext& ctx, const ToolCall& call) {
    // Hook: PRE_TOOL_CALL first — fires for every attempt, including
    // those that a precondition will reject. (E9, 2.0.6-rc19)
    if (fire_pre_tool_hook(ctx, call)) {
        auto msg = create_denied_message(call, "Cancelled by hook");
        fire_post_tool_hook(ctx, call, "", 0.0,
            ToolResultKind::rejected_precondition, msg);
        return {std::move(msg)};
    }

    auto pc = check_call_preconditions(ctx, call);
    if (pc.rejection.has_value()) {
        logger->info("Tool '{}' rejected by precondition (kind={})",
                     call.name, result_kind_to_string(pc.kind));
        fire_post_tool_hook(ctx, call, "", 0.0, pc.kind, *pc.rejection);
        return {std::move(*pc.rejection)};
    }

    auto exec_start = std::chrono::steady_clock::now();
    auto [msg, raw_result] = execute_tool(ctx, call);
    auto exec_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - exec_start).count();
    // #46 (v2.1.0): cap result content at LoopConfig.max_tool_result_bytes
    // so a single runaway tool can't exhaust the context budget. Cap=0
    // disables. Truncated form retains a "[... truncated, N more bytes]"
    // tail so downstream classification (#44) and identity prompts can
    // see that output was lost rather than the tool actually returning
    // a short result. Applied BEFORE classification so kind reflects
    // the bounded form, and BEFORE record_tool_call so the duplicate
    // cache stores what the model actually saw.
    apply_result_size_cap(msg.content);
    ctx.effective_tool_calls++;
    msg.metadata["added_at_iteration"] =
        std::to_string(ctx.metrics.iterations);
    record_tool_call(ctx, call, raw_result);

    // #44 (v2.1.0): honest byte-level signal so identity prompts can
    // teach pivot-on-empty rules and so error responses don't leak as
    // ok. Order matters — error trumps empty.
    ToolResultKind kind;
    if (mcp::looks_like_tool_error(msg.content)) {
        kind = ToolResultKind::error;
    } else if (mcp::is_effectively_empty(msg.content)) {
        kind = ToolResultKind::ok_empty;
    } else {
        kind = ToolResultKind::ok;
    }
    fire_post_tool_hook(ctx, call, raw_result, exec_ms, kind, msg);

    // Demo ask #5 (v2.1.0): anti-spiral primitive. Track consecutive
    // calls of the same tool regardless of arg similarity (exact-arg
    // duplicates are caught earlier by recent_tool_calls). When the
    // count reaches the configured threshold, populate
    // pending_anti_spiral_warning so the next turn's system reminder
    // tells the model to pivot tools or complete.
    update_anti_spiral_tracking(ctx, call.name);

    auto args_log = serialize_args(call);
    if (args_log.size() > 512) { args_log.resize(512); }
    logger->info("[tool_call] iter={} tier={} tool={} args={} "
                 "elapsed_ms={:.0f} result_chars={} status={}",
                 ctx.metrics.iterations,
                 ctx.locked_tier.empty() ? "lead" : ctx.locked_tier,
                 call.name, args_log, exec_ms,
                 raw_result.size(), result_kind_to_string(kind));

    extract_and_process_directives(ctx, raw_result);
    run_post_tool_hooks(ctx);

    return {std::move(msg)};
}

/**
 * @brief Fire PRE_TOOL_CALL hook.
 * @param ctx Loop context.
 * @param call Tool call.
 * @return true if hook cancelled.
 * @internal
 * @version 2.0.6-rc19
 */
bool ToolExecutor::fire_pre_tool_hook(
    const LoopContext& ctx, const ToolCall& call) {
    if (hook_iface_.fire_pre == nullptr) { return false; }
    auto json = build_pre_tool_json(call, ctx.locked_tier,
                                    ctx.metrics.iterations);
    char* mod = nullptr;
    int rc = hook_iface_.fire_pre(hook_iface_.registry,
        ENTROPIC_HOOK_PRE_TOOL_CALL, json.c_str(), &mod);
    free(mod);
    return rc != 0;
}

/**
 * @brief Cap tool-result content at LoopConfig.max_tool_result_bytes.
 *
 * Demo ask #6 (v2.1.0). See header for full contract.
 *
 * @internal
 * @version 2.1.1-rc1
 */
void ToolExecutor::apply_result_size_cap(std::string& content) const {
    mcp::truncate_to_cap(content, loop_config_.max_tool_result_bytes);
}

/**
 * @brief Update anti-spiral tracking after a tool dispatch.
 *
 * Demo ask #5 (v2.1.0). See header for full contract.
 *
 * @internal
 * @version 2.1.1-rc1
 */
void ToolExecutor::update_anti_spiral_tracking(
    LoopContext& ctx, const std::string& tool_name) {
    if (tool_name == ctx.last_tool_name) {
        ++ctx.consecutive_same_tool_calls;
    } else {
        ctx.last_tool_name = tool_name;
        ctx.consecutive_same_tool_calls = 1;
    }
    if (ctx.consecutive_same_tool_calls
            >= loop_config_.max_consecutive_same_tool) {
        ctx.pending_anti_spiral_warning =
            tool_name + " has been called "
            + std::to_string(ctx.consecutive_same_tool_calls)
            + " times consecutively; pivot to a different tool or "
              "complete the task next turn.";
    }
}

/**
 * @brief Fire POST_TOOL_CALL hook with result_kind; apply any transform.
 *
 * If the registered hook (or the last hook in a chain — last-write-wins
 * per registry semantics, see hook_registry_test.cpp::"Post-hook transforms
 * result") writes a non-null ``*modified_json``, that string replaces
 * ``msg.content``. Issue #2 (v2.1.1): pre-2.1.1 this freed the output
 * without ever applying it.
 *
 * @param ctx Loop context.
 * @param call Tool call.
 * @param raw_result Raw server response (may be empty on reject).
 * @param elapsed_ms Duration.
 * @param kind Typed outcome.
 * @param msg Message produced by this call; mutated if hook transforms.
 * @internal
 * @version 2.1.1
 */
void ToolExecutor::fire_post_tool_hook(
    const LoopContext& ctx, const ToolCall& call,
    const std::string& raw_result, double elapsed_ms,
    ToolResultKind kind, Message& msg) {
    if (hook_iface_.fire_post == nullptr) { return; }
    auto json = build_post_tool_json(
        call, raw_result, elapsed_ms, ctx.locked_tier,
        ctx.metrics.iterations, kind);
    char* out = nullptr;
    hook_iface_.fire_post(hook_iface_.registry,
        ENTROPIC_HOOK_POST_TOOL_CALL, json.c_str(), &out);
    if (out != nullptr) {
        msg.content = out;
        free(out);
    }
}

/**
 * @brief Check if batch should stop (circuit breaker or directives).
 * @param ctx Loop context.
 * @param results Results so far.
 * @return true if batch should stop.
 * @internal
 * @version 2.0.2
 */
bool ToolExecutor::should_stop_batch(
    const LoopContext& ctx,
    const std::vector<Message>& /*results*/) const {
    return ctx.state == AgentState::COMPLETE
        || ctx.pending_delegation.has_value()
        || ctx.pending_pipeline.has_value()
        || ctx.consecutive_duplicate_attempts >= 3;
}

/**
 * @brief Run post-tool hooks (after_tool callback).
 * @param ctx Loop context.
 * @internal
 * @version 1.8.5
 */
void ToolExecutor::run_post_tool_hooks(LoopContext& ctx) {
    if (hooks_.after_tool != nullptr) {
        hooks_.after_tool(ctx, hooks_.user_data);
    }
}

/**
 * @brief Create circuit breaker "stuck" message.
 * @return Feedback message.
 * @internal
 * @version 1.8.5
 */
Message ToolExecutor::create_circuit_breaker_message() {
    Message msg;
    msg.role = "user";
    msg.content =
        "STOP: You have called the same tool 3 times with "
        "identical arguments. This indicates you are stuck. "
        "Please try a completely different approach or respond "
        "to the user explaining what's blocking you.";
    logger->error("Circuit breaker triggered");
    return msg;
}

/**
 * @brief Create duplicate notification message.
 * @param call Duplicate tool call.
 * @param previous_result Previous result.
 * @return Feedback message.
 * @internal
 * @version 1.8.5
 */
Message ToolExecutor::create_duplicate_message(
    const ToolCall& call,
    const std::string& previous_result) {
    bool was_denied =
        previous_result.find("was denied") != std::string::npos
        || previous_result.find("not available") != std::string::npos;

    Message msg;
    msg.role = "user";

    if (was_denied) {
        msg.content =
            "Tool `" + call.name + "` is not available to you "
            "and retrying will not help. You MUST use a different "
            "approach. Do NOT call `" + call.name + "` again.";
    } else {
        msg.content =
            "Tool `" + call.name + "` was already called with "
            "the same arguments.\n\nPrevious result:\n" +
            previous_result +
            "\n\nDo NOT call this tool again. "
            "Use the previous result above.";
    }
    return msg;
}

/**
 * @brief Serialize tool call arguments to JSON string.
 *
 * Prefers arguments_json (preserves type info from interface_factory parse)
 * over the string-only arguments map. Without this, boolean/integer values
 * get serialized as strings and crash tools that expect typed values.
 *
 * @param call Tool call.
 * @return JSON string.
 * @internal
 * @version 2.0.4
 */
std::string ToolExecutor::serialize_args(const ToolCall& call) {
    if (!call.arguments_json.empty()) {
        return call.arguments_json;
    }
    nlohmann::json args;
    for (const auto& [k, v] : call.arguments) {
        args[k] = v;
    }
    return args.dump();
}

/**
 * @brief Serialize a tool call to JSON for callbacks.
 * @param call Tool call.
 * @return JSON string.
 * @internal
 * @version 1.8.5
 */
std::string ToolExecutor::serialize_tool_call(const ToolCall& call) {
    nlohmann::json j;
    j["id"] = call.id;
    j["name"] = call.name;
    j["arguments"] = nlohmann::json::object();
    for (const auto& [k, v] : call.arguments) {
        j["arguments"][k] = v;
    }
    return j.dump();
}

/**
 * @brief Fire tool complete callback.
 * @param call Tool call.
 * @param result Result text.
 * @param ms Duration in milliseconds.
 * @internal
 * @version 1.8.5
 */
void ToolExecutor::fire_tool_complete_callback(
    const ToolCall& call,
    const std::string& result,
    long long ms) {
    if (callbacks_.on_tool_complete == nullptr) {
        return;
    }
    auto call_json = serialize_tool_call(call);
    callbacks_.on_tool_complete(
        call_json.c_str(), result.c_str(),
        static_cast<double>(ms), callbacks_.user_data);
}

/**
 * @brief Build enriched POST_TOOL_CALL hook context JSON.
 * @param call Tool call that was executed.
 * @param raw_result Raw server response JSON (may be empty on reject).
 * @param elapsed_ms Execution duration in milliseconds.
 * @param tier Active tier.
 * @param iteration Loop iteration.
 * @param kind Typed outcome.
 * @return JSON: tool_name, args, result, directives, elapsed_ms, tier, iteration, result_kind.
 * @internal
 * @version 2.0.6-rc19
 */
std::string ToolExecutor::build_post_tool_json(
    const ToolCall& call,
    const std::string& raw_result,
    double elapsed_ms,
    const std::string& tier,
    int iteration,
    ToolResultKind kind) {
    nlohmann::json ctx;
    ctx["tool_name"] = call.name;
    ctx["args"] = nlohmann::json::parse(serialize_args(call));
    ctx["elapsed_ms"] = elapsed_ms;
    ctx["tier"] = tier.empty() ? std::string{"lead"} : tier;
    ctx["iteration"] = iteration;
    ctx["result_kind"] = result_kind_to_string(kind);
    try {
        auto sr = nlohmann::json::parse(raw_result);
        ctx["result"] = sr.value("result", raw_result);
        ctx["directives"] = sr.value(
            "directives", nlohmann::json::array());
    } catch (...) {
        ctx["result"] = raw_result;
        ctx["directives"] = nlohmann::json::array();
    }
    return ctx.dump();
}

/**
 * @brief Build PRE_TOOL_CALL hook context JSON.
 * @param call Tool call being attempted.
 * @param tier Active tier.
 * @param iteration Loop iteration.
 * @return JSON string.
 * @internal
 * @version 2.0.6-rc19
 */
std::string ToolExecutor::build_pre_tool_json(
    const ToolCall& call,
    const std::string& tier,
    int iteration) {
    nlohmann::json j;
    j["tool_name"] = call.name;
    j["args"] = nlohmann::json::parse(serialize_args(call));
    j["tier"] = tier.empty() ? std::string{"lead"} : tier;
    j["iteration"] = iteration;
    return j.dump();
}

/**
 * @brief Extract directives from ServerResponse JSON and process them.
 *
 * Parses the directives array from the raw tool result, constructs
 * typed Directive objects, and passes them to the engine's
 * DirectiveProcessor via the hooks callback.
 *
 * @param ctx Loop context (mutated by directive handlers).
 * @param raw_result ServerResponse JSON string.
 * @internal
 * @version 2.0.1
 */
/**
 * @brief Extract pipeline stage names from a result JSON object.
 * @param result_json Parsed result JSON.
 * @return Stage tier names (empty if absent).
 * @internal
 * @version 2.0.2
 */
static std::vector<std::string> extract_pipeline_stages(
    const nlohmann::json& result_json) {
    std::vector<std::string> stages;
    if (!result_json.contains("stages")) { return stages; }
    for (const auto& s : result_json["stages"]) {
        stages.push_back(s.get<std::string>());
    }
    return stages;
}

/**
 * @brief Build a typed Directive from a directive-descriptor JSON.
 * @param d Directive JSON ("type": ...).
 * @param result_json Parsed result JSON for parameter lookup.
 * @return Owned Directive (nullptr if type is unrecognized).
 * @internal
 * @version 2.0.2
 */
static std::unique_ptr<Directive> build_directive(
    const nlohmann::json& d, const nlohmann::json& result_json) {
    auto type_str = d.value("type", "");
    std::unique_ptr<Directive> result;
    if (type_str == "stop_processing") {
        result = std::make_unique<StopProcessingDirective>();
    } else if (type_str == "delegate") {
        result = std::make_unique<DelegateDirective>(
            result_json.value("target", ""),
            result_json.value("task", ""),
            result_json.value("max_turns", -1));
    } else if (type_str == "complete") {
        result = std::make_unique<CompleteDirective>(
            result_json.value("summary", ""));
    } else if (type_str == "pipeline") {
        result = std::make_unique<PipelineDirective>(
            extract_pipeline_stages(result_json),
            result_json.value("task", ""));
    }
    return result;
}

/**
 * @brief Pull the "directives" array out of a tool ServerResponse JSON.
 * @param raw_result Raw ServerResponse JSON string.
 * @return Pair of (resp object, directives reference). Returns
 *         (null, null) if the response is missing/empty/invalid.
 * @internal
 * @version 2.0.2
 */
static std::optional<std::pair<nlohmann::json, nlohmann::json>>
extract_directive_array(const std::string& raw_result) {
    auto resp = nlohmann::json::parse(raw_result, nullptr, false);
    if (!resp.is_object() || !resp.contains("directives")) {
        return std::nullopt;
    }
    auto dirs = resp["directives"];
    if (!dirs.is_array() || dirs.empty()) { return std::nullopt; }
    return std::make_pair(std::move(resp), std::move(dirs));
}

/**
 * @brief Extract directives from tool ServerResponse JSON and dispatch them.
 * @param ctx Loop context (mutated by directive handlers).
 * @param raw_result Raw ServerResponse JSON string.
 * @utility
 * @version 2.0.2
 */
void ToolExecutor::extract_and_process_directives(
    LoopContext& ctx, const std::string& raw_result) {
    if (hooks_.process_directives == nullptr) { return; }
    auto extracted = extract_directive_array(raw_result);
    if (!extracted) { return; }
    auto& [resp, dirs] = *extracted;

    auto result_json = nlohmann::json::parse(
        resp.value("result", "{}"), nullptr, false);

    std::vector<std::unique_ptr<Directive>> owned;
    for (const auto& d : dirs) {
        auto directive = build_directive(d, result_json);
        if (directive) { owned.push_back(std::move(directive)); }
    }
    if (owned.empty()) { return; }

    std::vector<const Directive*> ptrs;
    ptrs.reserve(owned.size());
    for (const auto& d : owned) { ptrs.push_back(d.get()); }
    logger->info("Processing {} directives from tool result", ptrs.size());
    hooks_.process_directives(ctx, ptrs, hooks_.user_data);
}

} // namespace entropic
