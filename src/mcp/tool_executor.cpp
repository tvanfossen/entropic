// SPDX-License-Identifier: Apache-2.0
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
 * @dg_internal
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
 * @brief Install the workspace server resolver (gh#166).
 * @param fn Resolver, or nullptr to clear.
 * @param user_data Forwarded to the resolver.
 * @req REQ-MCP-027
 * @version 2.13.0
 */
void ToolExecutor::set_server_resolver(
    ServerManager* (*fn)(const std::string&, void*), void* user_data) {
    server_resolver_ = fn;
    server_resolver_data_ = user_data;
}

/**
 * @brief The servers a session's tool call runs against (gh#166).
 * @param session_key Session the call belongs to.
 * @return The bound workspace's manager, else the constructed one.
 * @req REQ-MCP-027
 * @version 2.13.0
 */
ServerManager& ToolExecutor::servers_for(
    const std::string& session_key) const {
    if (server_resolver_ != nullptr) {
        auto* resolved = server_resolver_(session_key, server_resolver_data_);
        if (resolved != nullptr) { return *resolved; }
    }
    return server_manager_;
}

/**
 * @brief Set permission persistence interface.
 * @param persist Permission persist callbacks.
 * @dg_internal
 * @version 1.8.8
 */
void ToolExecutor::set_permission_persist(
    const PermissionPersistInterface& persist) {
    permission_persist_ = persist;
}

/**
 * @brief Process a batch of tool calls.
 *
 * Batch handling before per-call dispatch: entropic.delegate is sorted
 * last, then the batch is truncated to the effective per-turn limit —
 * a per-identity override when the context carries one, the global
 * LoopConfig value otherwise.
 *
 * @param ctx Loop context (provides effective_max_tool_calls_per_turn, P3-18).
 * @param tool_calls Tool calls from model output.
 * @return One result message per processed call, in processing order;
 *         short of the input count when the batch was truncated or a
 *         circuit-breaker/directive stopped it early.
 * @req REQ-MCP-012
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
 *
 * Stable, so every other call keeps the model's emitted order — only
 * delegate moves.
 *
 * @param calls Input tool calls.
 * @return A copy with entropic.delegate calls moved to the end and all
 *         other relative ordering preserved.
 * @req REQ-MCP-012
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
 *
 * A tool whose owning server opted out via skip_duplicate_check is
 * never treated as a duplicate — it must always run for its side effect.
 *
 * @param ctx Loop context holding the recent-call cache.
 * @param call Tool call.
 * @return The cached previous result when this exact tool+arguments key
 *         was already seen; an empty string when it is new or the tool
 *         is opted out. Errored calls are never cached, so a transient
 *         failure cannot permanently poison the call.
 * @req REQ-MCP-015
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
 *
 * A bare error invites a retry spiral, so the duplicate path answers
 * with corrective guidance instead, escalating to a circuit-breaker
 * message once the model has repeated itself three times.
 *
 * @param ctx Loop context (its duplicate-attempt counter is bumped).
 * @param call Duplicate tool call.
 * @param previous_result Result the earlier identical call produced.
 * @return The circuit-breaker "stuck" message from the third
 *         consecutive duplicate onward; otherwise the previous result
 *         plus do-not-call-again guidance.
 * @req REQ-MCP-015
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
 *
 * The last precondition gate. Auto-approval or an operator allow-list
 * match short-circuits; otherwise the engine's approval callback is
 * consulted. Fires the informational ON_PERMISSION_CHECK hook either
 * way.
 *
 * @param call Tool call.
 * @return true when the call may be dispatched — auto-approved,
 *         explicitly allowed, or approved via the callback; false only
 *         when no approval callback is wired, which is logged.
 * @req REQ-MCP-012
 * @req REQ-MCP-009
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
 * @brief Check tier allowed_tools restrictions. (gh#83, v2.5.2)
 * @param ctx Loop context.
 * @param call Tool call.
 * @return nullopt when the call may proceed — no locked tier, no wired
 *         map, no allowlist for the tier, or the tool is listed;
 *         otherwise a rejection Message naming both the tier and the
 *         tool, logged at warning level.
 * @req REQ-MCP-014
 * @req REQ-MCP-012
 * @version 2.5.2
 */
std::optional<Message> ToolExecutor::check_tier_allowed(
    const LoopContext& ctx, const ToolCall& call) const {
    // gh#83 (v2.5.2): enforce the locked tier's allowed_tools at
    // dispatch — not just at prompt-injection time. Without this a
    // model that emits an off-allowlist call (hallucinated, learned,
    // or cross-tier) has it dispatched normally; tier isolation was
    // advisory only. Pass through when: no tier locked, no map wired,
    // or the tier declares no allowlist (unrestricted).
    if (ctx.locked_tier.empty() || tier_allowed_tools_ == nullptr) {
        return std::nullopt;
    }
    auto it = tier_allowed_tools_->find(ctx.locked_tier);
    // Authorized when: tier has no allowlist entry, an empty allowlist
    // (unrestricted), or the call is in the list. Short-circuit eval
    // guards the `it->second` access against the end() case.
    bool authorized = it == tier_allowed_tools_->end()
                   || it->second.empty()
                   || std::find(it->second.begin(), it->second.end(),
                                call.name) != it->second.end();
    if (authorized) {
        return std::nullopt;
    }
    logger->warn("Tool '{}' not in tier '{}' allowed_tools — rejecting",
                 call.name, ctx.locked_tier);
    return create_denied_message(
        call, "tier '" + ctx.locked_tier + "' is not authorized to call '"
                  + call.name + "'");
}

/**
 * @brief Check required fields are present.
 * @param schema Parsed JSON Schema object.
 * @param args Parsed tool arguments.
 * @return An empty string when every declared required field is
 *         present; otherwise "Missing required argument: <name>" for
 *         the first absent one.
 * @req REQ-MCP-013
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
 * @return An empty string when the value is one of the declared
 *         options; otherwise a message naming the property, the
 *         offending value and the full set of valid options.
 * @req REQ-MCP-013
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
 * @return An empty string when the value matches the declared type;
 *         otherwise "Type mismatch for '<key>': expected <type>". An
 *         unrecognised type name reads as a mismatch.
 * @req REQ-MCP-013
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
 * @return An empty string when the value satisfies both the enum (if
 *         declared) and the type (if declared); otherwise the enum
 *         error, which is checked first, else the type error.
 * @req REQ-MCP-013
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
 * @return An empty string when the arguments satisfy the schema — and
 *         also when the schema is absent or unparseable, which reads as
 *         "nothing to validate" rather than throwing; otherwise the
 *         first violation's description, required fields before
 *         per-property constraints.
 * @req REQ-MCP-013
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
 * @brief Extract the "result" text from an MCP result JSON envelope.
 * @param result_json Raw result string from the server.
 * @return The envelope's `result` field when the string parses as JSON
 *         carrying one; otherwise the raw string verbatim, so a server
 *         that answered off-contract still surfaces its text.
 * @req REQ-MCP-002
 * @version 2.3.7
 */
static std::string parse_tool_result_text(const std::string& result_json) {
    try {
        auto j = nlohmann::json::parse(result_json);
        return j.value("result", result_json);
    } catch (...) {
        return result_json;
    }
}

/**
 * @brief Execute a single tool call.
 *
 * The dispatch itself, once every precondition has passed. The server's
 * answer crosses an inbound trust boundary, so it is UTF-8 sanitized
 * before anything downstream reads it, then unwrapped from the
 * ServerResponse envelope and recorded in the history ring buffer.
 *
 * @param ctx Loop context (tool-call metric incremented).
 * @param call Tool call.
 * @return A pair of the user-role result Message — content is the
 *         envelope's result text, metadata carries tool_call_id and
 *         tool_name — and the raw envelope JSON the directive
 *         extraction later parses.
 * @req REQ-MCP-002
 * @req REQ-MCP-020
 * @req REQ-MCP-027
 * @version 2.13.0
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
    // gh#166 (v2.13.0): route to the RUNNING SESSION's workspace. This is
    // the call that touches the filesystem, so it is the one that must
    // land in the right repository; the metadata lookups above answer
    // schema/permission questions that are workspace-invariant for the
    // built-in servers and fail SAFE (unknown → WRITE required, no schema
    // → no validation skip that grants anything).
    auto result_json = mcp::sanitize_utf8(
        servers_for(ctx.session_key).execute(call.name, args_json));
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(end - start).count();

    ctx.metrics.tool_calls++;

    std::string result_text = parse_tool_result_text(result_json);

    // P1-11 (2.0.6-rc16): stash into history ring buffer so the
    // constitutional validator revision prompt (and diagnostic tools)
    // can surface prior-iteration tool calls without re-reading
    // messages[].
    record_tool_history(call, args_json, result_text, ms,
                        ctx.metrics.iterations);

    fire_tool_complete_callback(call, result_text, ms);

    Message msg;
    msg.role = "user";
    msg.content = result_text;
    msg.metadata["tool_call_id"] = call.id;
    msg.metadata["tool_name"] = call.name;

    return {std::move(msg), result_json};
}

/**
 * @brief Stash a finished tool call in the history ring buffer.
 *
 * Assigns the monotonic sequence number and reduces the call to the
 * bounded diagnostic form: argument KEYS only (never values) and a
 * result summary capped at 200 characters.
 *
 * @param call The tool call.
 * @param args_json Serialized args.
 * @param result_text Parsed result text.
 * @param ms Elapsed milliseconds.
 * @param iteration Loop iteration.
 * @req REQ-MCP-020
 * @version 2.3.7
 */
void ToolExecutor::record_tool_history(const ToolCall& call,
                                       const std::string& args_json,
                                       const std::string& result_text,
                                       long long ms, int iteration) {
    ToolCallRecord rec;
    rec.sequence = ++history_seq_;
    rec.tool_name = call.name;
    rec.params_summary = summarize_params(args_json);
    rec.status = (result_text.rfind("error", 0) == 0)
        ? "error" : "success";
    rec.result_summary = truncate_result(result_text, 200);
    rec.elapsed_ms = static_cast<double>(ms);
    rec.iteration = iteration;
    history_.record(rec);
}

/**
 * @brief Generate duplicate detection key.
 *
 * Arguments go through a JSON object, whose keys are ordered, so two
 * calls that differ only in argument emission order share a key.
 *
 * @param call Tool call.
 * @return "<tool name>:<sorted arguments JSON>" — the identity a
 *         repeated call is recognised by.
 * @req REQ-MCP-015
 * @version 2.12.0
 */
std::string ToolExecutor::tool_call_key(const ToolCall& call) {
    // Sort arguments for consistent key. gh#143: seed an OBJECT, not a
    // default-constructed null — an argument-free call would otherwise
    // key as "<name>:null" instead of "<name>:{}".
    nlohmann::json args = nlohmann::json::object();
    for (const auto& [k, v] : call.arguments) {
        args[k] = v;
    }
    return call.name + ":" + args.dump();
}

/**
 * @brief Record tool call for duplicate detection.
 *
 * Error results are deliberately NOT cached, so a transient failure
 * does not permanently poison the call for the rest of the turn.
 *
 * @param ctx Loop context holding the recent-call cache.
 * @param call Tool call.
 * @param result Raw ServerResponse envelope (or bare text).
 * @req REQ-MCP-015
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
 *
 * A bare denial invites a retry spiral, so the text names the tool and
 * the reason and then steers explicitly away from retrying.
 *
 * @param call Tool call.
 * @param reason Denial reason.
 * @return A user-role Message reading "Tool `X` was denied: <reason>"
 *         followed by do-NOT-retry / use-a-different-approach guidance.
 * @req REQ-MCP-015
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
 * @return A user-role Message reading "Tool `X` failed with error: ..."
 *         followed by the RECOVERY block — check arguments, try a
 *         different approach, do not retry with the same arguments.
 * @req REQ-MCP-015
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
 * @dg_internal
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
 * @param calls Tool call vector, resized in place.
 * @param limit Effective limit (after per-identity override, P3-18) —
 *              a per-identity value may raise or lower it relative to
 *              the global LoopConfig setting.
 * @req REQ-MCP-012
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
 *
 * The tool's own declared access level is what the identity's key set is
 * measured against, and an unknown tool resolves to WRITE — so a bogus
 * name cannot slip past a READ-only key set.
 *
 * @param ctx Loop context (provides identity from locked_tier).
 * @param call Tool call.
 * @return nullopt when no authorization manager is wired, the identity
 *         is not enforced, or access is granted; otherwise a rejection
 *         Message naming the identity and the missing access level,
 *         steering the model to entropic.delegate.
 * @req REQ-MCP-011
 * @req REQ-MCP-010
 * @req REQ-MCP-012
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
 *
 * The last two gates in the fixed precondition order — duplicate
 * detection, then operator approval. A non-duplicate resets the
 * consecutive-duplicate counter.
 *
 * @param ctx Loop context.
 * @param call Tool call.
 * @return nullopt when the call is neither a duplicate nor unapproved;
 *         otherwise the duplicate-guidance message (or circuit-breaker
 *         message) or a permission-denied message.
 * @req REQ-MCP-012
 * @req REQ-MCP-015
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
 * @dg_internal
 * @version 1.9.4
 */
/**
 * @brief Validate tool arguments against schema constraints.
 *
 * An empty schema — tool not found, or a plugin descriptor with no
 * inputSchema — skips validation, and unparseable arguments are treated
 * as "nothing to validate" rather than throwing through dispatch.
 *
 * @param call Tool call to validate.
 * @return nullopt when the arguments satisfy the declared schema (or
 *         there is nothing to check); otherwise a denial Message naming
 *         the specific violation, logged at warning level. The tool is
 *         never invoked on the rejection path.
 * @req REQ-MCP-013
 * @req REQ-MCP-008
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
 *
 * Issue #14 (v2.1.4): the anti-spiral hard block fires FIRST, before
 * schema/auth/duplicate checks. Cheaper and short-circuits any tool
 * the engine has decided to refuse regardless of other outcomes.
 *
 * The fixed order is: anti-spiral hard block, argument-schema
 * validation, MCP key authorization, tier allowed_tools, duplicate
 * detection, operator approval. Whichever gate comes first wins when a
 * call violates several.
 *
 * @param ctx Loop context.
 * @param call Tool call.
 * @return A PreconditionCheck whose rejection is empty when every gate
 *         passed; otherwise the earliest gate's model-facing message
 *         paired with its categorical kind — rejected_anti_spiral,
 *         rejected_schema, rejected_precondition, rejected_unauthorized
 *         or rejected_duplicate — so hook consumers branch on an enum
 *         rather than on engine-authored prose.
 * @req REQ-MCP-012
 * @version 2.5.2
 */
PreconditionCheck ToolExecutor::check_call_preconditions(
    LoopContext& ctx, const ToolCall& call) {
    // Issue #14 (v2.1.4): anti-spiral hard block fires FIRST. Cheaper
    // than schema/auth checks and short-circuits a tool that the
    // engine has decided to refuse, regardless of whether the call
    // would otherwise pass other preconditions.
    PreconditionCheck pc = check_anti_spiral_hard_block(ctx, call);
    if (pc.rejection.has_value()) {
        return pc;
    }
    if (auto r = check_schema(call); r.has_value()) {
        pc.rejection = std::move(r);
        pc.kind = ToolResultKind::rejected_schema;
    } else if (auto a = check_mcp_authorization(ctx, call);
               a.has_value()) {
        pc.rejection = std::move(a);
        pc.kind = ToolResultKind::rejected_precondition;
    } else if (auto t = check_tier_allowed(ctx, call); t.has_value()) {
        pc.rejection = std::move(t);
        pc.kind = ToolResultKind::rejected_unauthorized;  // gh#83
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
 * @return An empty PreconditionCheck when the call is approved;
 *         otherwise a permission-denied Message with kind
 *         rejected_precondition.
 * @req REQ-MCP-012
 * @req REQ-MCP-009
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
 * Every exit path — pre-hook cancel, precondition rejection, and normal
 * execution — fires POST_TOOL_CALL with the outcome's typed kind, so no
 * path drops the hook.
 *
 * @param ctx Loop context.
 * @param call Tool call.
 * @return Exactly one Message: the executed tool's result, the
 *         hook-cancelled denial, or the precondition rejection — each
 *         carrying its result_kind in metadata.
 * @req REQ-MCP-017
 * @req REQ-MCP-012
 * @version 2.13.0
 */
std::vector<Message> ToolExecutor::process_single_call(
    LoopContext& ctx, const ToolCall& call) {
    // Hook: PRE_TOOL_CALL first — fires for every attempt, including
    // those that a precondition will reject. (E9, 2.0.6-rc19)
    if (fire_pre_tool_hook(ctx, call)) {
        auto msg = create_denied_message(call, "Cancelled by hook");
        // gh#84 (v2.5.1): stamp kind so the engine treats this as a
        // non-progress outcome for the thinking-budget reset.
        msg.metadata["result_kind"] =
            result_kind_to_string(ToolResultKind::rejected_precondition);
        fire_post_tool_hook(ctx, call, "", 0.0,
            ToolResultKind::rejected_precondition, msg);
        return {std::move(msg)};
    }

    auto pc = check_call_preconditions(ctx, call);
    if (pc.rejection.has_value()) {
        logger->info("Tool '{}' rejected by precondition (kind={})",
                     call.name, result_kind_to_string(pc.kind));
        // gh#84 (v2.5.1): stamp kind for the engine's budget decision.
        pc.rejection->metadata["result_kind"] =
            result_kind_to_string(pc.kind);
        fire_post_tool_hook(ctx, call, "", 0.0, pc.kind, *pc.rejection);
        return {std::move(*pc.rejection)};
    }

    auto exec_start = std::chrono::steady_clock::now();
    auto [msg, raw_result] = execute_tool(ctx, call);
    auto exec_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - exec_start).count();

    finalize_tool_call(ctx, call, msg, raw_result, exec_ms);

    return {std::move(msg)};
}

/**
 * @brief Classify a tool result by its content (error/empty/ok).
 * @param content Result content (already size-capped, so the kind
 *                describes the bounded form the model saw).
 * @return ToolResultKind::error for error-shaped content,
 *         ToolResultKind::ok_empty for byte-level-empty content,
 *         ToolResultKind::ok otherwise — error trumps empty trumps ok
 *         (#44).
 * @req REQ-MCP-019
 * @version 2.3.7
 */
static ToolResultKind classify_tool_result(const std::string& content) {
    if (mcp::looks_like_tool_error(content)) {
        return ToolResultKind::error;
    }
    if (mcp::is_effectively_empty(content)) {
        return ToolResultKind::ok_empty;
    }
    return ToolResultKind::ok;
}

/**
 * @brief Emit the per-tool-call info log line.
 * @param ctx Loop context.
 * @param call The tool call.
 * @param exec_ms Execution time (ms).
 * @param raw_result Raw server result (for size).
 * @param kind Classified result kind.
 * @dg_internal
 * @version 2.3.7
 */
void ToolExecutor::log_tool_call(LoopContext& ctx, const ToolCall& call,
                                 double exec_ms,
                                 const std::string& raw_result,
                                 ToolResultKind kind) {
    auto args_log = serialize_args(call);
    if (args_log.size() > 512) { args_log.resize(512); }
    logger->info("[tool_call] iter={} tier={} tool={} args={} "
                 "elapsed_ms={:.0f} result_chars={} status={}",
                 ctx.metrics.iterations,
                 ctx.locked_tier.empty() ? "lead" : ctx.locked_tier,
                 call.name, args_log, exec_ms,
                 raw_result.size(), result_kind_to_string(kind));
}

/**
 * @brief Post-execution processing for a tool call.
 * @param ctx Loop context.
 * @param call The tool call.
 * The inbound boundary: the size cap is applied FIRST, so the model, the
 * classifier and the duplicate cache all see the same bounded content;
 * then the result is classified, recorded and handed to the
 * POST_TOOL_CALL hook.
 *
 * @param ctx Loop context.
 * @param call The tool call.
 * @param[in,out] msg Result message (content may be capped, and may be
 *                rewritten by the POST_TOOL_CALL hook).
 * @param raw_result Raw server result string.
 * @param exec_ms Execution time (ms).
 * @req REQ-MCP-019
 * @req REQ-MCP-017
 * @req REQ-MCP-015
 * @version 2.5.1
 */
void ToolExecutor::finalize_tool_call(LoopContext& ctx, const ToolCall& call,
                                      Message& msg,
                                      const std::string& raw_result,
                                      double exec_ms) {
    // #46 (v2.1.0): cap result content at LoopConfig.max_tool_result_bytes
    // so a single runaway tool can't exhaust the context budget. Applied
    // BEFORE classification so kind reflects the bounded form, and BEFORE
    // record_tool_call so the duplicate cache stores what the model saw.
    apply_result_size_cap(msg.content);
    ctx.effective_tool_calls++;
    msg.metadata["added_at_iteration"] =
        std::to_string(ctx.metrics.iterations);
    record_tool_call(ctx, call, raw_result);

    // #44 (v2.1.0): honest byte-level signal — error trumps empty.
    ToolResultKind kind = classify_tool_result(msg.content);
    // gh#84 (v2.5.1): stamp the result kind on the message so the
    // engine can tell genuine progress (ok / ok_empty) from rejections
    // and errors when deciding whether to reset the thinking budget.
    msg.metadata["result_kind"] = result_kind_to_string(kind);
    fire_post_tool_hook(ctx, call, raw_result, exec_ms, kind, msg);

    // Demo ask #5 (v2.1.0): anti-spiral primitive. Track consecutive
    // same-tool calls; at threshold, populate pending_anti_spiral_warning
    // so the next turn's reminder tells the model to pivot or complete.
    update_anti_spiral_tracking(ctx, call.name);

    log_tool_call(ctx, call, exec_ms, raw_result, kind);

    extract_and_process_directives(ctx, raw_result);
    run_post_tool_hooks(ctx);
}

/**
 * @brief Fire PRE_TOOL_CALL hook.
 *
 * Fires for EVERY attempt, including ones a precondition will go on to
 * reject, carrying tool name, args, tier and iteration.
 *
 * @param ctx Loop context.
 * @param call Tool call.
 * @return true when the hook returned non-zero, cancelling the call
 *         before dispatch; false when no hook is wired or it allowed the
 *         call. Any string the pre-hook wrote is freed, not applied —
 *         only POST_TOOL_CALL may rewrite content.
 * @req REQ-MCP-017
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
 * @param content Result content, truncated in place; a configured cap of
 *                0 disables truncation entirely.
 * @req REQ-MCP-019
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
 * The soft half of anti-spiral: reaching max_consecutive_same_tool
 * populates a one-shot pending warning that the next system reminder
 * delivers, telling the model to pivot tools or complete. A different
 * tool name resets the counter.
 *
 * @param ctx Loop context (counter and pending warning mutated).
 * @param tool_name Fully-qualified name of the tool just dispatched.
 * @req REQ-MCP-016
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
 * @brief Compute the effective hard-block threshold (sentinel-aware).
 *
 * See declaration. Issue #14, v2.1.4.
 *
 * @return The configured max_consecutive_same_tool_hard_block verbatim
 *         when non-negative — so an operator can set it high enough to
 *         effectively disable the hard block while keeping the soft
 *         advisory; for the negative sentinel, the derived
 *         max_consecutive_same_tool + 2.
 * @req REQ-MCP-016
 * @version 2.1.4
 */
int ToolExecutor::effective_hard_block_threshold() const {
    int configured = loop_config_.max_consecutive_same_tool_hard_block;
    if (configured < 0) {
        configured = loop_config_.max_consecutive_same_tool + 2;
    }
    return configured;
}

/**
 * @brief Pre-dispatch anti-spiral hard block.
 *
 * Triggered when consecutive_same_tool_calls (already covering THIS
 * call, since it would be incremented by update_anti_spiral_tracking
 * after dispatch) would meet or exceed the effective hard threshold.
 * Computes the projected counter (+1 if same as last, else 1) without
 * mutating ctx. Returns a rejected_anti_spiral PreconditionCheck on
 * trip, default-constructed (no rejection) otherwise.
 *
 * Issue #14, v2.1.4.
 *
 * @param ctx Loop context (read-only — the projection does not mutate
 *            the counter).
 * @param call Tool call about to be dispatched.
 * @return An empty PreconditionCheck when the projected consecutive
 *         count is below the effective threshold; otherwise a rejection
 *         Message naming the tool, the count and the threshold, with
 *         kind rejected_anti_spiral — and the tool is not dispatched at
 *         all.
 * @req REQ-MCP-016
 * @req REQ-MCP-012
 * @version 2.1.4
 */
PreconditionCheck ToolExecutor::check_anti_spiral_hard_block(
    const LoopContext& ctx, const ToolCall& call) const {
    PreconditionCheck pc;
    int projected = (call.name == ctx.last_tool_name)
        ? (ctx.consecutive_same_tool_calls + 1)
        : 1;
    int threshold = effective_hard_block_threshold();
    if (projected >= threshold) {
        std::string text =
            "[anti-spiral] tool '" + call.name + "' blocked after "
            + std::to_string(projected)
            + " consecutive calls (threshold "
            + std::to_string(threshold)
            + "); pivot to a different tool or complete the task.";
        pc.rejection = create_denied_message(call, text);
        pc.kind = ToolResultKind::rejected_anti_spiral;
    }
    return pc;
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
 * @param ctx Loop context.
 * @param call Tool call.
 * @param raw_result Raw server response (may be empty on reject).
 * @param elapsed_ms Duration.
 * @param kind Typed outcome — computed BEFORE the hook fires and passed
 *            as an INPUT, so a transformed content may not match the
 *            kind downstream code carries. Intentional.
 * @param msg Message produced by this call; its content is REPLACED
 *            when the hook writes a non-null string, and left untouched
 *            when it writes NULL.
 * @req REQ-MCP-017
 * @version 2.9.7
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
        // gh#3 recurrence (gh#111, v2.9.7): a hook's transformed result is
        // an inbound boundary crossing a plugin .so, same class as
        // fire_post_generate_hook/fire_complete_hook in engine.cpp —
        // sanitize before it becomes msg.content.
        msg.content = mcp::sanitize_utf8(out);
        free(out);
    }
}

/**
 * @brief Check if batch should stop (circuit breaker or directives).
 * @param ctx Loop context.
 * @param results Results so far.
 * @return true if batch should stop.
 * @dg_internal
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
 * @dg_internal
 * @version 1.8.5
 */
void ToolExecutor::run_post_tool_hooks(LoopContext& ctx) {
    if (hooks_.after_tool != nullptr) {
        hooks_.after_tool(ctx, hooks_.user_data);
    }
}

/**
 * @brief Create circuit breaker "stuck" message.
 * @return A user-role Message telling the model it has repeated the
 *         same call three times, that this means it is stuck, and to
 *         try a different approach or explain the blocker — the
 *         escalation past ordinary duplicate guidance.
 * @req REQ-MCP-015
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
 * @param previous_result Result the earlier identical call produced.
 * @return A user-role Message: when the earlier result was itself a
 *         denial, text saying the tool is unavailable and retrying will
 *         not help; otherwise the previous result echoed back with an
 *         explicit "Do NOT call this tool again, use the previous
 *         result" instruction.
 * @req REQ-MCP-015
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
 * gh#143 (v2.12.0): the accumulator is seeded as an explicit OBJECT.
 * `nlohmann::json args;` default-constructs to NULL, and the loop below
 * only coerces it to an object when the map is non-empty — so an
 * argument-free call serialised to the four characters `null`.
 * `json::parse("null")` then SUCCEEDS (it is valid JSON), schema
 * validation waves it through whenever the tool declares no required
 * fields, and the first `.value()` inside the server throws
 * type_error.306 straight out of dispatch, killing the whole run.
 * `git.diff` with no arguments is a legitimate call shape, so this was
 * reachable from any model on any turn.
 *
 * The entrance is bounded (one function) where the exits are not (28
 * `.value()` sites across five servers), which is the guard architecture
 * decision #56 prescribes for a recurring defect class.
 *
 * @param call Tool call.
 * @return The parse-preserved arguments_json when the call carries one;
 *         otherwise the string-only arguments map serialised as a JSON
 *         object — `{}` when the map is empty, never `null`. This is the
 *         string schema validation, permission patterns and duplicate
 *         keys are all computed from.
 * @req REQ-MCP-013
 * @version 2.12.0
 */
std::string ToolExecutor::serialize_args(const ToolCall& call) {
    if (!call.arguments_json.empty()) {
        return call.arguments_json;
    }
    nlohmann::json args = nlohmann::json::object();
    for (const auto& [k, v] : call.arguments) {
        args[k] = v;
    }
    return args.dump();
}

/**
 * @brief Serialize a tool call to JSON for callbacks.
 * @param call Tool call.
 * @return JSON string.
 * @dg_internal
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
 * @dg_internal
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
 * @return The hook's context JSON carrying tool_name, args, result,
 *         directives, elapsed_ms, tier, iteration and result_kind — the
 *         typed kind being what lets consumers branch on an enum rather
 *         than grep engine-authored prose.
 * @req REQ-MCP-017
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
 * @param tier Active tier ("lead" when unset).
 * @param iteration Loop iteration.
 * @return The hook's context JSON carrying tool_name, args, tier and
 *         iteration — the four things a pre-hook needs to decide
 *         whether to cancel.
 * @req REQ-MCP-017
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
 * @dg_internal
 * @version 2.0.1
 */
/**
 * @brief Extract pipeline stage names from a result JSON object.
 * @param result_json Parsed result JSON.
 * @return Stage tier names (empty if absent).
 * @dg_internal
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
 *
 * Issue #10 (v2.1.4): the "complete" branch now extracts
 * coverage_gap / gap_description / suggested_files from the result
 * JSON and populates the typed CompleteDirective fields.
 *
 * @param d Directive JSON ("type": ...).
 * @param result_json Parsed result JSON for parameter lookup.
 * @return Owned Directive (nullptr if type is unrecognized).
 * @dg_internal
 * @version 2.1.6
 */
/**
 * @brief Build a CompleteDirective from a result JSON (issue #10).
 * @param result_json Tool result JSON.
 * @return An owned CompleteDirective carrying the summary plus the
 *         typed coverage_gap / gap_description / suggested_files
 *         fields; the optional fields default to false/empty when the
 *         tool did not emit them.
 * @req REQ-MCP-024
 * @version 2.3.7
 */
static std::unique_ptr<Directive> build_complete_directive(
    const nlohmann::json& result_json) {
    auto cd = std::make_unique<CompleteDirective>(
        result_json.value("summary", ""));
    cd->coverage_gap = result_json.value("coverage_gap", false);
    cd->gap_description = result_json.value("gap_description", "");
    if (result_json.contains("suggested_files")
        && result_json["suggested_files"].is_array()) {
        cd->suggested_files =
            result_json["suggested_files"].get<std::vector<std::string>>();
    }
    return cd;
}

/**
 * @brief Parse the `context` array of a delegate/pipeline result (gh#162).
 *
 * The MCP layer owns this because core.so takes typed structs, never JSON
 * (design decision #21). Entries without a `path` are skipped — the tool
 * boundary already dropped them, and a second reader must not resurrect
 * what the first refused.
 *
 * @param result_json Parsed tool result JSON.
 * @return Typed references, empty when the key is absent or malformed.
 * @utility
 * @req REQ-DELEG-006
 * @version 2.13.0
 */
static std::vector<ContextRef> extract_context_refs(
    const nlohmann::json& result_json) {
    std::vector<ContextRef> refs;
    if (!result_json.contains("context")
        || !result_json["context"].is_array()) {
        return refs;
    }
    for (const auto& entry : result_json["context"]) {
        if (!entry.is_object()) { continue; }
        ContextRef ref;
        ref.path = entry.value("path", std::string{});
        if (ref.path.empty()) { continue; }
        ref.lines = entry.value("lines", std::string{});
        ref.note = entry.value("note", std::string{});
        refs.push_back(std::move(ref));
    }
    return refs;
}

/**
 * @brief Build a Directive from a parsed directive + result JSON.
 * @param d Directive descriptor JSON carrying the wire "type" name.
 * @param result_json Parsed result JSON, source of the directive's
 *                    typed parameters.
 * @return An owned typed Directive for stop_processing, delegate,
 *         complete or pipeline; nullptr for any other wire name, which
 *         the caller then skips rather than dispatching.
 * @req REQ-MCP-002
 * @req REQ-MCP-024
 * @version 2.13.0
 */
static std::unique_ptr<Directive> build_directive(
    const nlohmann::json& d, const nlohmann::json& result_json) {
    auto type_str = d.value("type", "");
    std::unique_ptr<Directive> result;
    if (type_str == "stop_processing") {
        result = std::make_unique<StopProcessingDirective>();
    } else if (type_str == "delegate") {
        // gh#32 (v2.1.6): resume_delegation emits action=resume_delegation
        // with delegation_id but no target. The directive's target is
        // resolved later by the engine after loading the original
        // delegation's tier from storage.
        auto dl = std::make_unique<DelegateDirective>(
            result_json.value("target", ""),
            result_json.value("task", ""),
            result_json.value("max_turns", -1),
            result_json.value("delegation_id", ""));
        // gh#162 (v2.13.0): carry the lead's file references and the
        // resume-by-tier flag through to the engine.
        dl->context = extract_context_refs(result_json);
        dl->resume_by_target = result_json.value("resume_by_target", false);
        result = std::move(dl);
    } else if (type_str == "complete") {
        result = build_complete_directive(result_json);
    } else if (type_str == "pipeline") {
        auto pl = std::make_unique<PipelineDirective>(
            extract_pipeline_stages(result_json),
            result_json.value("task", ""));
        pl->context = extract_context_refs(result_json);  // gh#162
        result = std::move(pl);
    }
    return result;
}

/**
 * @brief Pull the "directives" array out of a tool ServerResponse JSON.
 * @param raw_result Raw ServerResponse JSON string.
 * @return The parsed envelope paired with its non-empty `directives`
 *         array; nullopt when the string does not parse as an object,
 *         carries no `directives` key, or the array is empty — the
 *         no-side-effect case, which needs no dispatch.
 * @req REQ-MCP-002
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
 *
 * The consumer side of the envelope contract: whichever server kind
 * answered, the directives array is read the same way and handed to the
 * engine's DirectiveProcessor as typed objects.
 *
 * @param ctx Loop context (mutated by directive handlers).
 * @param raw_result Raw ServerResponse JSON string.
 * @req REQ-MCP-002
 * @req REQ-MCP-024
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
