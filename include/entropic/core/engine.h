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
 * @version 1.8.6
 */

#pragma once

#include <entropic/core/compaction.h>
#include <entropic/core/context_manager.h>
#include <entropic/core/directives.h>
#include <entropic/core/engine_types.h>
#include <entropic/core/response_generator.h>
#include <entropic/interfaces/i_hook_handler.h>
#include <entropic/interfaces/i_inference_callbacks.h>

#include <atomic>
#include <filesystem>
#include <optional>
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
 * @version 1.8.6
 */
class AgentEngine {
public:
    /// @brief Max delegation nesting depth (0=root, 1=child, 2=max).
    /// @version 1.8.6
    static constexpr int MAX_DELEGATION_DEPTH = 2;

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
     * @brief Set the tier resolution interface for delegation.
     * @param tier_res Tier resolution callbacks (wired by facade).
     * @version 1.8.6
     */
    void set_tier_resolution(const TierResolutionInterface& tier_res);

    /**
     * @brief Set the storage interface for persistence.
     * @param storage Storage callbacks (wired by facade). Nullable — engine
     *        works without storage (in-memory only mode).
     * @version 1.8.8
     */
    void set_storage(const StorageInterface& storage);

    /**
     * @brief Set the hook dispatch interface.
     * @param hooks Hook dispatch interface (wired by facade).
     * @version 1.9.1
     */
    void set_hooks(const HookInterface& hooks);

    /**
     * @brief Run the engine loop on a pre-built context.
     *
     * Used by DelegationManager for child loops. Public so the
     * delegation manager (same .so) can invoke it.
     *
     * @param ctx Loop context to execute.
     * @version 1.8.6
     */
    void run_loop(LoopContext& ctx);

    /**
     * @brief Get the tier resolution interface.
     * @return Tier resolution interface reference.
     * @version 1.8.6
     */
    const TierResolutionInterface& tier_resolution() const;

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

    // ── Delegation (v1.8.6) ──────────────────────────────

    /**
     * @brief Execute a pending delegation after tool processing.
     * @param ctx Loop context with pending_delegation set.
     * @version 1.8.6
     */
    void execute_pending_delegation(LoopContext& ctx);           ///< @internal

    /**
     * @brief Execute a pending pipeline after tool processing.
     * @param ctx Loop context with pending_pipeline set.
     * @version 1.8.6
     */
    void execute_pending_pipeline(LoopContext& ctx);             ///< @internal

    /**
     * @brief Check if auto-chain should fire.
     * @param ctx Loop context.
     * @param finish_reason Generation finish reason.
     * @param content Response content.
     * @return true if auto-chain conditions met.
     * @version 1.8.6
     */
    bool should_auto_chain(const LoopContext& ctx,
                           const std::string& finish_reason,
                           const std::string& content);         ///< @internal

    /**
     * @brief Attempt auto-chain: child→COMPLETE, root→TierChange.
     * @param ctx Loop context.
     * @param finish_reason Generation finish reason.
     * @param content Response content.
     * @return true if auto-chain was triggered.
     * @version 1.8.6
     */
    bool try_auto_chain(LoopContext& ctx,
                        const std::string& finish_reason,
                        const std::string& content);            ///< @internal

    /**
     * @brief Fire a pre-hook and return true if cancelled.
     * @param point Hook point.
     * @param iteration Current iteration number.
     * @return true if hook cancelled the operation.
     * @version 1.9.1
     */
    bool fire_pre_hook(entropic_hook_point_t point, int iteration); ///< @internal

    /**
     * @brief Fire POST_GENERATE hook.
     * @param result Generation result.
     * @version 1.9.1
     */
    void fire_post_generate_hook(const GenerateResult& result);     ///< @internal

    /**
     * @brief Fire ON_DELEGATE pre-hook. Returns true if cancelled.
     * @param pending Delegation info.
     * @param depth Current delegation depth.
     * @return true if hook cancelled delegation.
     * @version 1.9.1
     */
    bool fire_delegate_pre_hook(const PendingDelegation& pending,
                                int depth);                         ///< @internal

    /**
     * @brief Fire ON_DELEGATE_COMPLETE post-hook.
     * @param target Target tier.
     * @param success Whether delegation succeeded.
     * @version 1.9.1
     */
    void fire_delegate_complete_hook(const std::string& target,
                                     bool success);                 ///< @internal

    /**
     * @brief Get or discover the project git repository.
     * @return Repo directory, or empty if not found.
     * @version 1.8.6
     */
    std::filesystem::path get_repo_dir();                       ///< @internal

    /**
     * @brief Initialize a git repo if none exists.
     * @param project_dir Directory to init.
     * @return true if repo now exists.
     * @version 1.8.6
     */
    bool init_project_repo(const std::filesystem::path& project_dir); ///< @internal

    /**
     * @brief Fire on_delegation_start callback.
     * @version 1.8.6
     */
    void fire_delegation_start(const LoopContext& ctx,
                               const std::string& tier,
                               const std::string& task);      ///< @internal

    /**
     * @brief Fire on_delegation_complete callback.
     * @version 1.8.6
     */
    void fire_delegation_complete(const LoopContext& ctx,
                                  const std::string& tier,
                                  const struct DelegationResult& result); ///< @internal

    // ── Members ──────────────────────────────────────────
    InferenceInterface inference_;                       ///< Inference contract
    LoopConfig loop_config_;                             ///< Loop config
    EngineCallbacks callbacks_;                          ///< Event callbacks
    std::atomic<bool> interrupt_flag_{false};             ///< Hard interrupt
    std::atomic<bool> pause_flag_{false};                 ///< Pause signal
    std::unordered_map<std::string, std::string> context_anchors_; ///< Persistent anchors
    ToolExecutionInterface tool_exec_;                     ///< Tool execution (v1.8.5)
    TierResolutionInterface tier_res_;                    ///< Tier resolution (v1.8.6)
    StorageInterface storage_;                             ///< Storage persistence (v1.8.8)
    DirectiveProcessor directive_processor_;              ///< Directive dispatch
    TokenCounter token_counter_;                          ///< Token counting
    CompactionManager compaction_manager_;                ///< Compaction
    ContextManager context_manager_;                      ///< Context management
    ResponseGenerator response_generator_;                ///< Response generation
    HookInterface hooks_;                                    ///< Hook dispatch (v1.9.1)
    std::optional<std::filesystem::path> cached_repo_dir_; ///< Cached repo path (v1.8.6)
    bool repo_dir_checked_ = false;                        ///< Repo discovery done (v1.8.6)
};

} // namespace entropic
