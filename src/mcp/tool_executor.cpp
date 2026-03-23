/**
 * @file tool_executor.cpp
 * @brief ToolExecutor implementation.
 * @version 1.8.5
 */

#include <entropic/mcp/tool_executor.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>

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
 * @param ctx Loop context.
 * @param tool_calls Tool calls from model output.
 * @return Result messages.
 * @internal
 * @version 1.8.5
 */
std::vector<Message> ToolExecutor::process_tool_calls(
    LoopContext& ctx,
    const std::vector<ToolCall>& tool_calls) {
    logger->info("Processing {} tool calls", tool_calls.size());
    ctx.state = AgentState::WAITING_TOOL;
    fire_state_callback(ctx);

    auto limited = sort_tool_calls(tool_calls);
    truncate_to_limit(limited);

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
 * @version 1.8.5
 */
bool ToolExecutor::check_approval(const ToolCall& call) {
    auto args_json = serialize_args(call);
    bool auto_ok = loop_config_.auto_approve_tools
                || server_manager_.is_explicitly_allowed(
                       call.name, args_json);
    if (auto_ok) {
        return true;
    }

    // No callback = headless → DENY (per adversarial review #1)
    if (callbacks_.on_tool_call == nullptr) {
        logger->warn("No approval callback — denying: {}", call.name);
        return false;
    }

    // Invoke approval callback (MVP: treat invocation as approval)
    auto call_json = serialize_tool_call(call);
    callbacks_.on_tool_call(call_json.c_str(),
                            callbacks_.user_data);
    return true;
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
 * @brief Execute a single tool call.
 * @param ctx Loop context.
 * @param call Tool call.
 * @return (result message, raw result string).
 * @internal
 * @version 1.8.5
 */
std::pair<Message, std::string> ToolExecutor::execute_tool(
    LoopContext& ctx, const ToolCall& call) {

    auto args_json = serialize_args(call);
    logger->info("[TOOL CALL] {}", call.name);

    if (callbacks_.on_tool_start != nullptr) {
        auto call_json = serialize_tool_call(call);
        callbacks_.on_tool_start(call_json.c_str(),
                                 callbacks_.user_data);
    }

    auto start = std::chrono::steady_clock::now();
    auto result_json = server_manager_.execute(
        call.name, args_json);
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

    logger->info("[TOOL COMPLETE] {} -> {} chars ({}ms)",
                 call.name, result_text.size(), ms);

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
 * @brief Truncate tool calls to max_tool_calls_per_turn.
 * @param calls Tool call vector (mutated).
 * @internal
 * @version 1.8.5
 */
void ToolExecutor::truncate_to_limit(
    std::vector<ToolCall>& calls) const {
    auto limit = static_cast<size_t>(
        loop_config_.max_tool_calls_per_turn);
    if (calls.size() > limit) {
        calls.resize(limit);
    }
}

/**
 * @brief Check preconditions (duplicate, approval, tier).
 * @param ctx Loop context.
 * @param call Tool call.
 * @return Rejection message if blocked, nullopt if clear.
 * @internal
 * @version 1.8.5
 */
std::optional<Message> ToolExecutor::check_call_preconditions(
    LoopContext& ctx, const ToolCall& call) {
    auto dup_result = check_duplicate(ctx, call);
    if (!dup_result.empty()) {
        return handle_duplicate(ctx, call, dup_result);
    }
    ctx.consecutive_duplicate_attempts = 0;

    if (!check_approval(call)) {
        return create_denied_message(call, "Permission denied");
    }
    return check_tier_allowed(ctx, call);
}

/**
 * @brief Process a single tool call (precondition check + execute).
 * @param ctx Loop context.
 * @param call Tool call.
 * @return Result messages (0 or 1).
 * @internal
 * @version 1.8.5
 */
std::vector<Message> ToolExecutor::process_single_call(
    LoopContext& ctx, const ToolCall& call) {

    auto rejection = check_call_preconditions(ctx, call);
    if (rejection.has_value()) {
        return {std::move(*rejection)};
    }

    auto [msg, raw_result] = execute_tool(ctx, call);
    ctx.effective_tool_calls++;
    msg.metadata["added_at_iteration"] =
        std::to_string(ctx.metrics.iterations);
    record_tool_call(ctx, call, raw_result);

    run_post_tool_hooks(ctx);

    return {std::move(msg)};
}

/**
 * @brief Check if batch should stop (circuit breaker or directives).
 * @param ctx Loop context.
 * @param results Results so far.
 * @return true if batch should stop.
 * @internal
 * @version 1.8.5
 */
bool ToolExecutor::should_stop_batch(
    const LoopContext& ctx,
    const std::vector<Message>& /*results*/) const {
    return ctx.consecutive_duplicate_attempts >= 3;
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
 * @param call Tool call.
 * @return JSON string.
 * @internal
 * @version 1.8.5
 */
std::string ToolExecutor::serialize_args(const ToolCall& call) {
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

} // namespace entropic
