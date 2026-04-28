// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file engine_types.h
 * @brief Types for the agentic loop engine.
 *
 * LoopConfig, LoopContext, LoopMetrics, EngineCallbacks, GenerationEvents,
 * InterruptContext, InterruptMode, ToolApproval. Used across engine
 * subsystems (AgentEngine, ResponseGenerator, ContextManager).
 *
 * @version 1.8.4
 */

#pragma once

#include <entropic/core/directives.h>
#include <entropic/types/enums.h>
#include <entropic/types/message.h>
#include <entropic/types/tool_call.h>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief C++ enum class for agent execution states.
 *
 * Maps 1:1 to C entropic_agent_state_t for cross-boundary use.
 *
 * @version 1.8.0
 */
enum class AgentState : int {
    IDLE          = ENTROPIC_AGENT_STATE_IDLE,
    PLANNING      = ENTROPIC_AGENT_STATE_PLANNING,
    EXECUTING     = ENTROPIC_AGENT_STATE_EXECUTING,
    WAITING_TOOL  = ENTROPIC_AGENT_STATE_WAITING_TOOL,
    VERIFYING     = ENTROPIC_AGENT_STATE_VERIFYING,
    DELEGATING    = ENTROPIC_AGENT_STATE_DELEGATING,
    COMPLETE      = ENTROPIC_AGENT_STATE_COMPLETE,
    ERROR         = ENTROPIC_AGENT_STATE_ERROR,
    INTERRUPTED   = ENTROPIC_AGENT_STATE_INTERRUPTED,
    PAUSED        = ENTROPIC_AGENT_STATE_PAUSED,
};

/**
 * @brief How to handle generation interrupt.
 * @version 1.8.4
 */
enum class InterruptMode {
    CANCEL,  ///< Discard partial response, stop
    PAUSE,   ///< Keep partial response, await input
    INJECT,  ///< Keep partial, inject context, continue
};

/**
 * @brief Tool approval responses from user.
 * @version 1.8.4
 */
enum class ToolApproval {
    DENY,         ///< Deny this once
    ALLOW,        ///< Allow this once
    ALWAYS_DENY,  ///< Deny and save to config
    ALWAYS_ALLOW, ///< Allow and save to config
};

/**
 * @brief Configuration for the agentic loop.
 * @version 1.8.4
 */
struct LoopConfig {
    int max_iterations = 15;            ///< Max loop iterations before forced stop
    int max_consecutive_errors = 3;     ///< Errors before ERROR state
    int max_tool_calls_per_turn = 10;   ///< Tool calls per iteration (v1.8.5)
    int idle_timeout_seconds = 300;     ///< Idle timeout (reserved)
    int context_length = 16384;         ///< Context budget for compaction (v2.0.4)
    bool require_plan_for_complex = true; ///< Planning gate (reserved)
    bool stream_output = true;          ///< Stream vs batch generation
    bool auto_approve_tools = false;    ///< Skip tool approval (v1.8.5)
    /// @brief Anti-spiral threshold: after N consecutive calls of the
    /// SAME tool (regardless of arg similarity, since exact-arg
    /// duplicates are already handled by recent_tool_calls), the
    /// engine populates pending_anti_spiral_warning so the next turn's
    /// system reminder tells the model to pivot tools or complete.
    /// (Demo ask #5, v2.1.0)
    int max_consecutive_same_tool = 5;
};

/**
 * @brief Metrics collected during loop execution.
 * @version 1.8.4
 */
struct LoopMetrics {
    int iterations = 0;     ///< Total iterations completed
    int tool_calls = 0;     ///< Total tool calls executed
    int tokens_used = 0;    ///< Total tokens consumed
    int errors = 0;         ///< Total errors encountered
    double start_time = 0.0; ///< Loop start (seconds since epoch)
    double end_time = 0.0;  ///< Loop end (seconds since epoch)

    /**
     * @brief Get loop duration in milliseconds.
     * @return Duration in ms.
     * @version 1.8.4
     */
    int duration_ms() const;
};

/**
 * @brief Pending single delegation (stored by dir_delegate handler).
 * @version 1.8.6
 */
struct PendingDelegation {
    std::string target;    ///< Target tier name
    std::string task;      ///< Task description
    int max_turns = -1;    ///< Max turns for child (-1 = default)
};

/**
 * @brief Pending multi-stage pipeline (stored by dir_pipeline handler).
 * @version 1.8.6
 */
struct PendingPipeline {
    std::vector<std::string> stages; ///< Tier names in order
    std::string task;                ///< Task description
};

/**
 * @brief Resolved tier information for building child delegation contexts.
 * @version 2.0.6-rc16
 */
struct ChildContextInfo {
    std::string system_prompt;              ///< Built for target tier
    std::vector<std::string> tools;         ///< Tool JSON definitions for tier
    bool explicit_completion = false;       ///< Requires entropic.complete?
    std::string completion_instructions;    ///< Instructions for explicit completion
    bool valid = false;                     ///< False if tier not found
    int max_iterations_override = -1;          ///< Per-tier max_iterations (-1 = global, P3-18)
    int max_tool_calls_per_turn_override = -1; ///< Per-tier tool cap (-1 = global, P3-18)
};

/**
 * @brief Tier resolution callbacks for delegation and auto-chain.
 *
 * Injected by the facade. Allows core.so to resolve tier identity
 * information without depending on prompts.so or inference.so.
 *
 * @version 1.8.6
 */
struct TierResolutionInterface {
    /// @brief Build context info for a child delegation to the given tier.
    ChildContextInfo (*resolve_tier)(
        const std::string& tier_name, void* user_data) = nullptr;

    /// @brief Get a string parameter from tier identity frontmatter.
    /// @return Parameter value, or empty string if not found.
    std::string (*get_tier_param)(
        const std::string& tier_name,
        const std::string& param_name,
        void* user_data) = nullptr;

    /// @brief Get valid handoff targets from a tier.
    std::vector<std::string> (*get_handoff_targets)(
        const std::string& tier_name, void* user_data) = nullptr;

    /// @brief Check if a tier exists in the configuration.
    bool (*tier_exists)(
        const std::string& tier_name, void* user_data) = nullptr;

    void* user_data = nullptr; ///< Opaque pointer (facade context)
};

/**
 * @brief Mutable state carried through the agentic loop.
 *
 * All mutable loop state lives here. The engine itself is stateless
 * between run() calls (except context_anchors which persist).
 *
 * @version 2.0.6-rc16
 */
struct LoopContext {
    std::vector<Message> messages;                         ///< Conversation history
    std::vector<ToolCall> pending_tool_calls;              ///< Pending tool calls (v1.8.5)
    AgentState state = AgentState::IDLE;                   ///< Current state
    LoopMetrics metrics;                                   ///< Timing and counts
    int consecutive_errors = 0;                            ///< Error streak counter
    int consecutive_duplicate_attempts = 0;                ///< Stuck-model detector
    int effective_tool_calls = 0;                          ///< Non-blocked calls this iteration
    bool has_pending_tool_results = false;                 ///< Tool results awaiting presentation
    std::string locked_tier;                               ///< Tier locked for this loop ("" = none)
    std::string task_id;                                   ///< External task ID (MCP integration)
    std::string conversation_id;                             ///< Conversation ID for storage (v1.8.8)
    std::string source = "human";                          ///< Message source
    std::vector<std::string> all_tools;                    ///< Full tool list as raw JSON strings
    std::string base_system;                               ///< Base system prompt (pre-tier formatting)
    std::unordered_map<std::string, std::string> metadata; ///< Runtime metadata
    int delegation_depth = 0;                              ///< 0 = root, 1+ = child
    std::vector<std::string> delegation_ancestor_tiers;    ///< Tier stack from root to this loop (P1-9, 2.0.6-rc16)
    std::string parent_conversation_id;                    ///< Parent conv ID (delegation)
    std::vector<std::string> child_conversation_ids;       ///< Spawned child IDs
    std::string active_phase = "default";                  ///< Active inference phase
    std::unordered_map<std::string, std::string> recent_tool_calls; ///< Duplicate detection cache (v1.8.5)
    std::optional<PendingDelegation> pending_delegation;  ///< Stored by dir_delegate (v1.8.6)
    std::optional<PendingPipeline> pending_pipeline;      ///< Stored by dir_pipeline (v1.8.6)
    int effective_max_iterations = -1;           ///< Per-identity override (-1 = LoopConfig, P3-18)
    int effective_max_tool_calls_per_turn = -1;  ///< Per-identity override (-1 = LoopConfig, P3-18)
    /// @brief One-shot reminder text consumed by the next per-turn
    /// system prompt assembly. Engine populates after a rejected
    /// validation; ResponseGenerator emits as a "[engine] previous
    /// turn rejected: …" line and the engine clears it post-emit.
    /// (Demo ask #2, v2.1.0)
    std::string pending_validation_feedback;

    /// @brief One-shot anti-spiral reminder for the next per-turn
    /// system prompt. Populated by ToolExecutor when
    /// consecutive_same_tool_calls reaches LoopConfig
    /// .max_consecutive_same_tool; ResponseGenerator emits as a
    /// "[engine] anti-spiral: <tool> called N times consecutively;
    /// pivot or complete next turn." line; engine clears post-emit.
    /// (Demo ask #5, v2.1.0)
    std::string pending_anti_spiral_warning;

    /// @brief Name of the most recently dispatched tool. Used to
    /// detect runs of the same tool for the anti-spiral primitive.
    /// Reset to "" when a different tool runs.
    /// (Demo ask #5, v2.1.0)
    std::string last_tool_name;

    /// @brief Count of CONSECUTIVE successful calls to the same tool
    /// (last_tool_name). Reset to 0 when a different tool runs.
    /// Compared against LoopConfig::max_consecutive_same_tool.
    /// (Demo ask #5, v2.1.0)
    int consecutive_same_tool_calls = 0;
};

/**
 * @brief Callback function pointer types for engine events.
 *
 * All callbacks are optional (nullptr = no-op). The engine checks
 * for nullptr before invoking. user_data pointers are passed through
 * unchanged — the engine never dereferences them.
 *
 * @version 1.8.4
 */
struct EngineCallbacks {
    void (*on_state_change)(int state, void* ud) = nullptr;            ///< AgentState as int
    void (*on_stream_chunk)(const char* chunk, size_t len,
                            void* ud) = nullptr;                       ///< Per-token streaming
    void (*on_tier_selected)(const char* tier, void* ud) = nullptr;    ///< Tier routing result
    void (*on_routing_complete)(const char* json, void* ud) = nullptr; ///< Full routing JSON
    void (*on_tool_call)(const char* json, void* ud) = nullptr;        ///< Tool call request
    void (*on_tool_start)(const char* json, void* ud) = nullptr;       ///< Tool execution start
    void (*on_tool_complete)(const char* json, const char* result,
                             double ms, void* ud) = nullptr;           ///< Tool execution done
    void (*on_presenter_notify)(const char* key, const char* json,
                                void* ud) = nullptr;                   ///< UI notification
    void (*on_compaction)(const char* json, void* ud) = nullptr;       ///< Compaction result
    void (*on_pause_prompt)(const char* partial, char** injection,
                            void* ud) = nullptr;                       ///< Pause: get injection
    void (*on_tool_record)(const char* tier, const char* json,
                           const char* result, const char* error,
                           double ms, void* ud) = nullptr;             ///< Tool audit record
    void (*on_delegation_start)(const char* child_id, const char* tier,
                                const char* task, void* ud) = nullptr; ///< Delegation spawned
    void (*on_delegation_complete)(const char* child_id, const char* tier,
                                   const char* summary, int success,
                                   void* ud) = nullptr;                ///< Delegation returned
    const char* (*error_sanitizer)(const char* raw, void* ud) = nullptr; ///< Sanitize errors
    void* user_data = nullptr; ///< Opaque pointer passed to all callbacks
};

/**
 * @brief Tool execution callback type.
 *
 * Called by the engine when tool calls are parsed from model output.
 * The facade wires this to ToolExecutor::process_tool_calls().
 *
 * @param ctx Mutable loop context.
 * @param tool_calls Tool calls to process.
 * @param user_data Opaque pointer (ToolExecutor instance).
 * @return Result messages to append to context.
 * @version 1.8.5
 */
using ToolExecutionFn = std::vector<Message> (*)(
    LoopContext& ctx,
    const std::vector<ToolCall>& tool_calls,
    void* user_data);

/**
 * @brief Tool execution interface for the engine.
 *
 * Injected by the facade. Allows core.so to process tool calls
 * without depending on mcp.so.
 *
 * @version 1.8.5
 */
struct ToolExecutionInterface {
    ToolExecutionFn process_tool_calls = nullptr; ///< Dispatches tool calls
    void* user_data = nullptr;                     ///< Opaque pointer (ToolExecutor*)

    /**
     * @brief Optional: return a compact JSON summary of recent tool
     *        calls (for validator retry enrichment / diagnostics).
     *
     * Caller owns the returned C string (free via ToolExecutionFree).
     * Nullptr means the executor has no history surface.
     * (P1-11, 2.0.6-rc16)
     */
    char* (*history_json)(size_t count, void* user_data) = nullptr;

    /// @brief Free function for strings returned by history_json.
    void (*free_fn)(char*) = nullptr;
};

/**
 * @brief Engine-level hooks called during tool processing.
 *
 * Bridges ToolExecutor (mcp.so) to DirectiveProcessor (core.so).
 * Defined in engine_types.h because it only uses core types.
 *
 * @version 2.0.2
 */
struct ToolExecutorHooks {
    /// @brief Called after each tool execution.
    /// @version 1.8.5
    void (*after_tool)(LoopContext& ctx, void* user_data) = nullptr;

    /// @brief Process directives from tool results.
    /// @version 1.8.5
    DirectiveResult (*process_directives)(
        LoopContext& ctx,
        const std::vector<const Directive*>& directives,
        void* user_data) = nullptr;

    void* user_data = nullptr; ///< Opaque pointer for hooks
};

/**
 * @brief Storage interface for conversation persistence.
 *
 * Injected by the facade. Allows core.so to persist conversations,
 * delegation records, and compaction snapshots without depending
 * on storage.so. All callbacks are optional (nullptr = no-op).
 *
 * @version 1.8.8
 */
struct StorageInterface {
    /// @brief Save a compaction snapshot (full history before compaction).
    /// @param conversation_id Conversation to snapshot.
    /// @param messages_json JSON array of all messages.
    /// @param user_data Opaque pointer.
    /// @return true on success.
    bool (*save_snapshot)(
        const char* conversation_id,
        const char* messages_json,
        void* user_data) = nullptr;

    /// @brief Create a delegation record with child conversation.
    /// @param parent_id Parent conversation ID.
    /// @param delegating_tier Source tier.
    /// @param target_tier Target tier.
    /// @param task Task description.
    /// @param max_turns Turn limit (0 = unlimited).
    /// @param[out] delegation_id Created delegation ID.
    /// @param[out] child_conversation_id Created child conversation ID.
    /// @param user_data Opaque pointer.
    /// @return true on success.
    bool (*create_delegation)(
        const char* parent_id,
        const char* delegating_tier,
        const char* target_tier,
        const char* task,
        int max_turns,
        std::string& delegation_id,
        std::string& child_conversation_id,
        void* user_data) = nullptr;

    /// @brief Complete a delegation record.
    /// @param delegation_id Delegation ID.
    /// @param status "completed" or "failed".
    /// @param summary Result summary (may be nullptr).
    /// @param user_data Opaque pointer.
    /// @return true on success.
    bool (*complete_delegation)(
        const char* delegation_id,
        const char* status,
        const char* summary,
        void* user_data) = nullptr;

    /// @brief Save messages to a conversation.
    /// @param conversation_id Conversation ID.
    /// @param messages_json JSON array of message objects.
    /// @param user_data Opaque pointer.
    /// @return true on success.
    bool (*save_conversation)(
        const char* conversation_id,
        const char* messages_json,
        void* user_data) = nullptr;

    void* user_data = nullptr; ///< Opaque pointer (storage backend)
};

/**
 * @brief Permission persistence callback type.
 *
 * Called when a user approves ALWAYS_ALLOW or ALWAYS_DENY
 * so the pattern can be saved to disk. Injected by the facade.
 *
 * @param pattern Permission pattern string.
 * @param allow true for allow, false for deny.
 * @param user_data Opaque pointer (PermissionPersister*).
 * @version 1.8.8
 */
using PermissionPersistFn = void (*)(
    const char* pattern, bool allow, void* user_data);

/**
 * @brief Permission persistence interface.
 * @version 1.8.8
 */
struct PermissionPersistInterface {
    PermissionPersistFn persist = nullptr; ///< Persist callback
    void* user_data = nullptr;             ///< Opaque pointer
};

/**
 * @brief Atomic flags for interrupt/pause signaling.
 *
 * Passed to ResponseGenerator. UI thread writes, generation thread reads.
 *
 * @version 1.8.4
 */
struct GenerationEvents {
    std::atomic<bool>* interrupt = nullptr; ///< Hard interrupt flag
    std::atomic<bool>* pause = nullptr;     ///< Pause flag
};

/**
 * @brief Context for interrupted/paused generation.
 * @version 1.8.4
 */
struct InterruptContext {
    std::string partial_content;             ///< Content generated before interrupt
    std::vector<ToolCall> partial_tool_calls; ///< Parsed tool calls (if any)
    std::string injection;                   ///< User injection content
    InterruptMode mode = InterruptMode::PAUSE; ///< Interrupt handling mode
};

/**
 * @brief Get the string name for an AgentState value.
 * @param state Agent state.
 * @return Static string. Never NULL.
 * @utility
 * @version 1.8.4
 */
const char* agent_state_name(AgentState state);

} // namespace entropic
