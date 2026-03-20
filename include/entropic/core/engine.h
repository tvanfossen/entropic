/**
 * @file engine.h
 * @brief Core agent execution engine.
 *
 * Manages the agentic loop lifecycle: state transitions, generation,
 * context management, directive processing, interrupt/pause.
 *
 * @par Lifecycle
 * @code
 *   AgentEngine engine(inference, loop_config, compaction_config);
 *   engine.set_callbacks(callbacks);
 *   auto result = engine.run(messages_json);
 * @endcode
 *
 * @par Threading
 * - run() is synchronous and blocks the calling thread.
 * - interrupt()/pause()/cancel_pause() are thread-safe (atomic flags).
 * - Callbacks fire on the calling thread.
 *
 * @version 1.8.4
 */

#pragma once

#include <entropic/core/compaction.h>
#include <entropic/core/context_manager.h>
#include <entropic/core/directives.h>
#include <entropic/core/engine_types.h>
#include <entropic/core/response_generator.h>
#include <entropic/interfaces/i_inference_callbacks.h>

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Core agent execution engine.
 *
 * Owns all engine subsystems: ResponseGenerator, ContextManager,
 * CompactionManager, DirectiveProcessor. Implements the agentic
 * loop (plan-act-observe-repeat) with proper state management
 * and termination conditions.
 *
 * @version 1.8.4
 */
class AgentEngine {
public:
    /**
     * @brief Construct an agent engine.
     * @param inference Inference interface (function pointers).
     * @param loop_config Loop configuration.
     * @param compaction_config Compaction configuration.
     * @version 1.8.4
     */
    AgentEngine(const InferenceInterface& inference,
                const LoopConfig& loop_config,
                const CompactionConfig& compaction_config);

    /**
     * @brief Run the engine on a set of messages.
     * @param messages Initial messages (system + user).
     * @return Final messages including all generated content.
     * @version 1.8.4
     */
    std::vector<Message> run(std::vector<Message> messages);

    /**
     * @brief Set callback functions for loop events.
     * @param callbacks Callback configuration.
     * @version 1.8.4
     */
    void set_callbacks(const EngineCallbacks& callbacks);

    /**
     * @brief Set the tool execution interface.
     * @param tool_exec Tool execution interface (wired by facade).
     * @version 1.8.5
     */
    void set_tool_executor(const ToolExecutionInterface& tool_exec);

    /**
     * @brief Interrupt the running loop (thread-safe).
     * @version 1.8.4
     */
    void interrupt();

    /**
     * @brief Reset interrupt flag for next run.
     * @version 1.8.4
     */
    void reset_interrupt();

    /**
     * @brief Pause generation (thread-safe).
     * @version 1.8.4
     */
    void pause();

    /**
     * @brief Cancel pause and interrupt completely.
     * @version 1.8.4
     */
    void cancel_pause();

    /**
     * @brief Get context usage for a message list.
     * @param messages Message list.
     * @return (tokens_used, max_tokens).
     * @version 1.8.4
     */
    std::pair<int, int> context_usage(
        const std::vector<Message>& messages) const;

private:
    /**
     * @brief Main loop implementation.
     * @param ctx Loop context.
     * @version 1.8.4
     */
    void loop(LoopContext& ctx);

    /**
     * @brief Execute a single loop iteration.
     * @param ctx Loop context.
     * @version 1.8.4
     */
    void execute_iteration(LoopContext& ctx);

    /**
     * @brief Evaluate decision when no tool calls were effective.
     * @param ctx Loop context.
     * @param content Response content.
     * @param finish_reason Generation finish reason.
     * @version 1.8.4
     */
    void evaluate_no_tool_decision(LoopContext& ctx,
                                   const std::string& content,
                                   const std::string& finish_reason);

    /**
     * @brief Check if loop should stop.
     * @param ctx Loop context.
     * @return true if loop should terminate.
     * @version 1.8.4
     */
    bool should_stop(const LoopContext& ctx) const;

    /**
     * @brief Set agent state and fire callback.
     * @param ctx Loop context.
     * @param state New state.
     * @version 1.8.4
     */
    void set_state(LoopContext& ctx, AgentState state);

    /**
     * @brief Reinject all cached context anchors.
     * @param ctx Loop context.
     * @version 1.8.4
     */
    void reinject_context_anchors(LoopContext& ctx);

    /**
     * @brief Register all directive handlers.
     * @version 1.8.4
     */
    void register_directive_handlers();

    // ── Directive handlers ───────────────────────────────
    void dir_stop(LoopContext&, const Directive&, DirectiveResult&);         ///< @internal
    void dir_tier_change(LoopContext&, const Directive&, DirectiveResult&);  ///< @internal
    void dir_delegate(LoopContext&, const Directive&, DirectiveResult&);     ///< @internal
    void dir_pipeline(LoopContext&, const Directive&, DirectiveResult&);     ///< @internal
    void dir_complete(LoopContext&, const Directive&, DirectiveResult&);     ///< @internal
    void dir_clear_todos(LoopContext&, const Directive&, DirectiveResult&);  ///< @internal
    void dir_inject(LoopContext&, const Directive&, DirectiveResult&);       ///< @internal
    void dir_prune(LoopContext&, const Directive&, DirectiveResult&);        ///< @internal
    void dir_anchor(LoopContext&, const Directive&, DirectiveResult&);       ///< @internal
    void dir_phase(LoopContext&, const Directive&, DirectiveResult&);        ///< @internal
    void dir_notify(LoopContext&, const Directive&, DirectiveResult&);       ///< @internal

    /**
     * @brief Parse tool calls from model output via adapter.
     * @param raw_content Raw model output.
     * @return Pair of (cleaned content, parsed tool calls).
     * @version 1.8.5
     */
    std::pair<std::string, std::vector<ToolCall>> parse_tool_calls(
        const std::string& raw_content);

    /**
     * @brief Process tool calls and handle directives.
     * @param ctx Loop context.
     * @param tool_calls Parsed tool calls.
     * @version 1.8.5
     */
    void process_tool_results(LoopContext& ctx,
                              const std::vector<ToolCall>& tool_calls);

    // ── Members ──────────────────────────────────────────
    InferenceInterface inference_;                       ///< Inference contract
    LoopConfig loop_config_;                             ///< Loop config
    EngineCallbacks callbacks_;                          ///< Event callbacks
    std::atomic<bool> interrupt_flag_{false};             ///< Hard interrupt
    std::atomic<bool> pause_flag_{false};                 ///< Pause signal
    std::unordered_map<std::string, std::string> context_anchors_; ///< Persistent anchors
    ToolExecutionInterface tool_exec_;                     ///< Tool execution (v1.8.5)
    DirectiveProcessor directive_processor_;              ///< Directive dispatch
    TokenCounter token_counter_;                          ///< Token counting
    CompactionManager compaction_manager_;                ///< Compaction
    ContextManager context_manager_;                      ///< Context management
    ResponseGenerator response_generator_;                ///< Response generation
};

} // namespace entropic
