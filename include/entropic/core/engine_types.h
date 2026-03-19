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

#include <entropic/types/enums.h>
#include <entropic/types/message.h>
#include <entropic/types/tool_call.h>

#include <atomic>
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
    bool require_plan_for_complex = true; ///< Planning gate (reserved)
    bool stream_output = true;          ///< Stream vs batch generation
    bool auto_approve_tools = false;    ///< Skip tool approval (v1.8.5)
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
 * @brief Mutable state carried through the agentic loop.
 *
 * All mutable loop state lives here. The engine itself is stateless
 * between run() calls (except context_anchors which persist).
 *
 * @version 1.8.4
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
    std::string source = "human";                          ///< Message source
    std::vector<std::string> all_tools;                    ///< Full tool list as raw JSON strings
    std::string base_system;                               ///< Base system prompt (pre-tier formatting)
    std::unordered_map<std::string, std::string> metadata; ///< Runtime metadata
    int delegation_depth = 0;                              ///< 0 = root, 1+ = child
    std::string parent_conversation_id;                    ///< Parent conv ID (delegation)
    std::vector<std::string> child_conversation_ids;       ///< Spawned child IDs
    std::string active_phase = "default";                  ///< Active inference phase
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
