// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file tool_executor.h
 * @brief Processes tool calls from model output.
 *
 * Handles approval flow, duplicate detection, circuit breaker,
 * tier allowlist enforcement, and bash_commands restriction.
 * Subsystem of AgentEngine — receives shared dependencies by reference.
 *
 * @version 1.9.4
 */

#pragma once

#include <entropic/core/directives.h>
#include <entropic/core/engine_types.h>
#include <entropic/mcp/mcp_authorization.h>
#include <entropic/mcp/server_manager.h>

#include <optional>
#include <string>
#include <vector>

namespace entropic {

/* ToolExecutorHooks moved to engine_types.h — v2.0.2 */

/**
 * @brief Processes tool calls from model output.
 *
 * Handles approval flow, duplicate detection, circuit breaker,
 * tier allowlist enforcement, and bash_commands restriction.
 * Subsystem of AgentEngine.
 *
 * @version 1.8.5
 */
class ToolExecutor {
public:
    /**
     * @brief Construct with shared dependencies.
     * @param server_manager Server manager for tool execution.
     * @param loop_config Loop configuration.
     * @param callbacks Shared callbacks.
     * @param hooks Optional engine-level hooks.
     * @version 1.8.5
     */
    ToolExecutor(ServerManager& server_manager,
                 const LoopConfig& loop_config,
                 EngineCallbacks& callbacks,
                 ToolExecutorHooks hooks = {});

    /**
     * @brief Set permission persistence interface.
     * @param persist Permission persist callbacks (wired by facade).
     * @version 1.8.8
     */
    void set_permission_persist(const PermissionPersistInterface& persist);

    /**
     * @brief Process a batch of tool calls.
     * @param ctx Mutable loop context.
     * @param tool_calls Tool calls from model output.
     * @return Vector of result messages to append to context.
     * @version 1.8.5
     */
    std::vector<Message> process_tool_calls(
        LoopContext& ctx,
        const std::vector<ToolCall>& tool_calls);

private:
    /**
     * @brief Sort tool calls so entropic.delegate is always last.
     * @param calls Input tool calls.
     * @return Sorted copy.
     * @version 1.8.5
     */
    static std::vector<ToolCall> sort_tool_calls(
        const std::vector<ToolCall>& calls);

    /**
     * @brief Extract directives from ServerResponse and process via hooks.
     * @param ctx Loop context (mutated by directive handlers).
     * @param raw_result ServerResponse JSON string.
     * @version 2.0.1
     */
    void extract_and_process_directives(
        LoopContext& ctx, const std::string& raw_result);

    /**
     * @brief Check duplicate tool call. Returns previous result or empty.
     * @param ctx Loop context.
     * @param call Tool call to check.
     * @return Previous result string, or empty if not duplicate.
     * @version 1.8.5
     */
    std::string check_duplicate(const LoopContext& ctx,
                                const ToolCall& call) const;

    /**
     * @brief Handle duplicate: increment counter, build feedback message.
     * @param ctx Loop context (mutated).
     * @param call The duplicate tool call.
     * @param previous_result Previous result text.
     * @return Feedback message.
     * @version 1.8.5
     */
    Message handle_duplicate(LoopContext& ctx,
                             const ToolCall& call,
                             const std::string& previous_result);

    /**
     * @brief Check tool approval (auto/allow-list/callback/deny).
     * @param call Tool call to check.
     * @return true if approved.
     * @version 1.8.5
     */
    bool check_approval(const ToolCall& call);

    /**
     * @brief Check tier allowed_tools + bash_commands restrictions.
     * @param ctx Loop context.
     * @param call Tool call to check.
     * @return Rejection message if blocked, nullopt if allowed.
     * @version 1.8.5
     */
    std::optional<Message> check_tier_allowed(
        const LoopContext& ctx, const ToolCall& call) const;

    /**
     * @brief Execute a single tool call through ServerManager.
     * @param ctx Loop context (mutated: metrics, effective_tool_calls).
     * @param call Tool call to execute.
     * @return Pair of (result message, raw result string).
     * @version 1.8.5
     */
    std::pair<Message, std::string> execute_tool(
        LoopContext& ctx, const ToolCall& call);

    /**
     * @brief Generate duplicate detection key: name + sorted args.
     * @param call Tool call.
     * @return Key string.
     * @version 1.8.5
     */
    static std::string tool_call_key(const ToolCall& call);

    /**
     * @brief Record tool call for duplicate detection (skip errors).
     * @param ctx Loop context (mutated: recent_tool_calls map in metadata).
     * @param call Tool call.
     * @param result Result string.
     * @version 1.8.5
     */
    void record_tool_call(LoopContext& ctx,
                          const ToolCall& call,
                          const std::string& result);

    /**
     * @brief Create a permission denied message.
     * @param call Tool call that was denied.
     * @param reason Denial reason.
     * @return Feedback message.
     * @version 1.8.5
     */
    static Message create_denied_message(const ToolCall& call,
                                         const std::string& reason);

    /**
     * @brief Create a tool error message.
     * @param call Tool call that failed.
     * @param error Error description.
     * @return Feedback message.
     * @version 1.8.5
     */
    static Message create_error_message(const ToolCall& call,
                                        const std::string& error);

    /**
     * @brief Fire state change callback.
     * @param ctx Loop context.
     * @internal
     * @version 1.8.5
     */
    void fire_state_callback(const LoopContext& ctx);

    /**
     * @brief Truncate to max_tool_calls_per_turn.
     * @param calls Tool calls (mutated).
     * @internal
     * @version 1.8.5
     */
    void truncate_to_limit(std::vector<ToolCall>& calls) const;

    /**
     * @brief Check preconditions (MCP keys, tier, duplicate, approval).
     * @param ctx Loop context.
     * @param call Tool call.
     * @return Rejection message if blocked, nullopt if clear.
     * @internal
     * @version 1.9.4
     */
    std::optional<Message> check_call_preconditions(
        LoopContext& ctx, const ToolCall& call);

    /**
     * @brief Validate tool arguments against schema constraints.
     * @param call Tool call to validate.
     * @return Rejection message, or nullopt on pass.
     * @version 2.0.6
     */
    std::optional<Message> check_schema(const ToolCall& call);

    /**
     * @brief Check duplicate detection and global approval.
     * @param ctx Loop context.
     * @param call Tool call.
     * @return Rejection message if blocked, nullopt if clear.
     * @internal
     * @version 1.9.4
     */
    std::optional<Message> check_dup_or_approval(
        LoopContext& ctx, const ToolCall& call);

    /**
     * @brief Process a single tool call (precondition check + execute).
     * @param ctx Loop context.
     * @param call Tool call.
     * @return Result messages.
     * @internal
     * @version 1.8.5
     */
    std::vector<Message> process_single_call(
        LoopContext& ctx, const ToolCall& call);

    /**
     * @brief Check if batch should stop.
     * @param ctx Loop context.
     * @param results Results so far.
     * @return true if stop.
     * @internal
     * @version 1.8.5
     */
    bool should_stop_batch(const LoopContext& ctx,
                           const std::vector<Message>& results) const;

    /**
     * @brief Run post-tool hooks.
     * @param ctx Loop context.
     * @internal
     * @version 1.8.5
     */
    void run_post_tool_hooks(LoopContext& ctx);

    /**
     * @brief Create circuit breaker message.
     * @return Feedback message.
     * @internal
     * @version 1.8.5
     */
    static Message create_circuit_breaker_message();

    /**
     * @brief Create duplicate notification message.
     * @param call Duplicate tool call.
     * @param previous_result Previous result.
     * @return Feedback message.
     * @internal
     * @version 1.8.5
     */
    static Message create_duplicate_message(
        const ToolCall& call,
        const std::string& previous_result);

    /**
     * @brief Serialize tool call arguments to JSON.
     * @param call Tool call.
     * @return JSON string.
     * @internal
     * @version 1.8.5
     */
    static std::string serialize_args(const ToolCall& call);

    /**
     * @brief Serialize full tool call to JSON.
     * @param call Tool call.
     * @return JSON string.
     * @internal
     * @version 1.8.5
     */
    static std::string serialize_tool_call(const ToolCall& call);

    /**
     * @brief Build enriched POST_TOOL_CALL hook context JSON.
     * @param call Tool call that was executed.
     * @param raw_result Raw server response JSON.
     * @param elapsed_ms Execution duration in milliseconds.
     * @return JSON string with tool_name, args, result, directives, elapsed_ms.
     * @internal
     * @version 1.9.5
     */
    static std::string build_post_tool_json(
        const ToolCall& call,
        const std::string& raw_result,
        double elapsed_ms);

    /**
     * @brief Fire tool complete callback.
     * @param call Tool call.
     * @param result Result text.
     * @param ms Duration in milliseconds.
     * @internal
     * @version 1.8.5
     */
    void fire_tool_complete_callback(const ToolCall& call,
                                     const std::string& result,
                                     long long ms);

    ServerManager& server_manager_;       ///< Server manager reference
    const LoopConfig& loop_config_;       ///< Loop configuration
    EngineCallbacks& callbacks_;          ///< Shared callbacks
    ToolExecutorHooks hooks_;             ///< Engine hooks
    PermissionPersistInterface permission_persist_; ///< Permission persistence (v1.8.8)
    HookInterface hook_iface_;            ///< Hook dispatch (v1.9.1)

public:
    /**
     * @brief Set the hook dispatch interface.
     * @param hooks Hook dispatch interface.
     * @utility
     * @version 1.9.1
     */
    void set_hooks(const HookInterface& hooks) { hook_iface_ = hooks; }

    /**
     * @brief Set the MCP authorization manager.
     * @param auth_mgr Authorization manager (must outlive ToolExecutor).
     * @utility
     * @version 1.9.4
     */
    void set_authorization_manager(MCPAuthorizationManager* auth_mgr) {
        auth_mgr_ = auth_mgr;
    }

private:
    MCPAuthorizationManager* auth_mgr_ = nullptr; ///< MCP key authorization (v1.9.4)

    /**
     * @brief Check MCP authorization for a tool call.
     * @param ctx Loop context (provides current identity).
     * @param call Tool call to check.
     * @return Error message if denied, nullopt if authorized.
     * @internal
     * @version 1.9.4
     */
    std::optional<Message> check_mcp_authorization(
        const LoopContext& ctx,
        const ToolCall& call) const;
};

} // namespace entropic
