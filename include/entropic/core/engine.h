// SPDX-License-Identifier: LGPL-3.0-or-later
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
#include <entropic/core/stream_think_filter.h>
#include <entropic/interfaces/i_inference_callbacks.h>
#include <entropic/types/session_logger.h>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
     * @brief Set the global stream observer.
     *
     * Fires for every token from every generation path — batch
     * entropic_run, entropic_run_streaming, and delegate child-loop
     * generations. Persists across per-call EngineCallbacks reassignment.
     *
     * @param observer Token callback (nullable clears).
     * @param user_data Forwarded to observer.
     * @version 2.0.6-rc16
     */
    void set_stream_observer(TokenCallback observer, void* user_data);

    /**
     * @brief Register a JSON provider for the validation block in
     *        ON_COMPLETE hook context.
     *
     * The callback returns a malloc'd JSON object describing the
     * validator's most recent result:
     *   {ran, verdict, violations, revisions_applied}
     * The engine copies and frees the returned buffer. NULL return
     * or nullptr callback means no validation block is emitted.
     * (E3, 2.0.6-rc17)
     *
     * @param provider JSON builder (nullable clears).
     * @param user_data Forwarded to provider.
     * @version 2.0.6-rc17
     */
    void set_validation_provider(char* (*provider)(void*),
                                 void* user_data);

    /**
     * @brief Register a callback invoked alongside interrupt().
     *
     * Used to propagate Ctrl+C into external MCP transports so
     * in-flight tool calls do not run to completion. Facade wires
     * this to ServerManager::interrupt_external_tools().
     * (P1-10, 2.0.6-rc16)
     *
     * @param cb Callback (nullable).
     * @param user_data Forwarded to cb.
     * @version 2.0.6-rc16
     */
    void set_external_interrupt(void (*cb)(void* user_data),
                                void* user_data);

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

    /**
     * @brief Get mutable reference to engine callbacks.
     *
     * Used by ToolExecutor which holds a reference that survives
     * set_callbacks() reassignment (same pattern as ResponseGenerator).
     *
     * @return Reference to internal callbacks member.
     * @utility
     * @version 2.0.1
     */
    EngineCallbacks& callbacks() { return callbacks_; }

    /**
     * @brief Get loop configuration.
     * @return Const reference to loop config.
     * @utility
     * @version 2.0.1
     */
    const LoopConfig& loop_config() const { return loop_config_; }

    /**
     * @brief Get directive processor for external hook wiring.
     * @return Reference to internal directive processor.
     * @utility
     * @version 2.0.1
     */
    DirectiveProcessor& directive_processor() { return directive_processor_; }

    /**
     * @brief Get metrics from the most recent completed run.
     *
     * Returns a copy of the LoopMetrics snapped at the end of the last
     * call to run() or run_turn(). Zero-initialized before any run has
     * completed. Thread-safe: read-only copy is returned. (P2-15)
     *
     * @return LoopMetrics from last run.
     * @utility
     * @version 2.0.6-rc16
     */
    LoopMetrics last_loop_metrics() const { return last_metrics_; }

    /**
     * @brief Per-tier aggregated metrics since engine start.
     *
     * Keys are tier names ("lead", "eng", etc.). Values accumulate
     * iterations/tool_calls/tokens_used/errors and sum duration_ms
     * across every run_loop entered with that locked_tier. Empty map
     * before any run. (P2-15 follow-up, 2.0.6-rc16.2)
     *
     * @return Copy of the per-tier metrics map.
     * @utility
     * @version 2.0.6-rc16.2
     */
    std::unordered_map<std::string, LoopMetrics>
        per_tier_metrics() const { return per_tier_metrics_; }

    // ── Conversation state (v2.0.2) ─────────────────────────

    /**
     * @brief Set the system prompt for conversation state.
     * @param prompt Assembled system prompt string.
     * @version 2.0.2
     */
    void set_system_prompt(const std::string& prompt);

    /**
     * @brief Set session logger for model transcript logging.
     * @param logger Non-owning pointer (nullable). Must outlive engine.
     * @version 2.0.2
     */
    void set_session_logger(SessionLogger* logger);

    /**
     * @brief Run a single conversation turn (stateful).
     *
     * Manages conversation history internally. Appends user message,
     * runs the agentic loop, appends result messages.
     *
     * @param input User input string.
     * @return Full result messages from engine.
     * @version 2.0.2
     */
    std::vector<Message> run_turn(const std::string& input);

    /**
     * @brief Run a streaming conversation turn (stateful).
     *
     * Same as run_turn but with streaming token output. Owns the
     * StreamThinkFilter, cancel polling, and session logger wiring.
     *
     * @param input User input string.
     * @param on_token Consumer token callback (receives filtered UTF-8).
     * @param user_data Consumer callback context.
     * @param cancel_flag Polled per-token, nullable.
     * @return 0 on success, 1 if cancelled, 2 on error.
     * @version 2.0.2
     */
    int run_streaming(const std::string& input,
                      TokenCallback on_token,
                      void* user_data,
                      int* cancel_flag);

    /**
     * @brief Clear conversation history.
     * @version 2.0.2
     */
    void clear_conversation();

    /**
     * @brief Get conversation message count.
     * @return Number of messages.
     * @version 2.0.2
     */
    size_t message_count() const;

    /**
     * @brief Get conversation messages (read-only).
     * @return Const reference to message vector.
     * @version 2.0.2
     */
    const std::vector<Message>& get_messages() const;

    // ── Directive hooks (v2.0.2) ────────────────────────────

    /**
     * @brief Build ToolExecutorHooks wired to this engine's DirectiveProcessor.
     *
     * Returns hooks with process_directives bridged to directive_processor().
     * Eliminates the need for facade bridge functions.
     *
     * @return Configured ToolExecutorHooks.
     * @version 2.0.2
     */
    ToolExecutorHooks build_directive_hooks();

    // ── Tier info (v2.0.2) ──────────────────────────────────

    /**
     * @brief Store pre-resolved tier context info.
     *
     * Called at configure time. The engine uses this data for
     * delegation instead of external TierResolutionInterface callbacks.
     *
     * @param name Tier name.
     * @param info Pre-resolved context info (system prompt, tools, etc.).
     * @version 2.0.2
     */
    void set_tier_info(const std::string& name,
                       const ChildContextInfo& info);

    /**
     * @brief Mark a tier as relay-on-single-delegate.
     *
     * When this tier is the active (lead) tier and exactly one delegate
     * completes successfully, the delegate's summary is used as the
     * final output without lead re-generating. Avoids redundant
     * re-synthesis of already user-ready delegate responses.
     *
     * @param name Tier name (typically "lead").
     * @version 2.0.11
     */
    void set_relay_single_delegate(const std::string& name);

    /**
     * @brief Store handoff rules for tier delegation.
     * @param rules Map of source tier → valid target tiers.
     * @version 2.0.2
     */
    void set_handoff_rules(
        const std::unordered_map<std::string,
            std::vector<std::string>>& rules);

    /**
     * @brief Check if a tier requires an explicit entropic.complete /
     *        entropic.delegate tool call to conclude its turn.
     *        Exposed for unit tests. (P1-6, 2.0.6-rc16)
     *
     * @param tier Tier name.
     * @return true if the tier has explicit_completion=true.
     * @utility
     * @version 2.0.6-rc16
     */
    bool tier_requires_explicit_completion(
        const std::string& tier) const;

    /**
     * @brief Check whether pending delegation would close a cycle.
     *        Exposed for unit tests. (P1-9, 2.0.6-rc16)
     *
     * @param ctx Loop context (ancestor chain + locked tier).
     * @param target Proposed delegation target tier.
     * @return true if target is already in the ancestor chain
     *         or matches the active locked_tier.
     * @utility
     * @version 2.0.6-rc16
     */
    bool is_delegation_cycle(
        const LoopContext& ctx, const std::string& target) const;

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
     * @brief Short-circuit for interrupted / length finish reasons.
     * @param ctx Loop context.
     * @param finish_reason Generation finish reason.
     * @return true if handled (caller should return).
     * @utility
     * @version 2.0.6-rc16
     */
    bool handle_terminal_finish_reasons(
        LoopContext& ctx, const std::string& finish_reason);

    /**
     * @brief Record failure when explicit_completion is required but
     *        zero tool calls were emitted. (P1-6)
     * @param ctx Loop context.
     * @param finish_reason Generation finish reason.
     * @return true if failure was recorded.
     * @utility
     * @version 2.0.6-rc16
     */
    bool record_explicit_completion_failure(
        LoopContext& ctx, const std::string& finish_reason);

    /**
     * @brief Decide post-delegation engine state.
     *
     * Runs the relay_single_delegate validator pass when applicable,
     * then transitions the loop to COMPLETE or EXECUTING based on
     * delegation success + explicit_completion requirement.
     *
     * @param ctx Loop context.
     * @param result Delegation outcome.
     * @utility
     * @version 2.0.6-rc16
     */
    void finalize_delegation_result(
        LoopContext& ctx, const struct DelegationResult& result);

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
     *
     * Builds a context JSON containing content, tier, a tool call
     * manifest (names + result sizes from messages), and the tier's
     * system prompt. The validator uses these to avoid false positives
     * on grounded responses and to maintain identity during revision.
     *
     * @param result Generation result (mutable — hooks may transform output).
     * @param tier Active tier name at the time of generation.
     * @param messages Current conversation messages (for tool manifest + system prompt).
     * @version 2.0.7
     */
    void fire_post_generate_hook(GenerateResult& result,
                                    const std::string& tier,
                                    const std::vector<Message>& messages); ///< @internal

    /**
     * @brief Fire ON_COMPLETE pre-hook for summary validation.
     *
     * Fires when entropic.complete is called. Context includes summary,
     * tier, and tool results. Hook can cancel (reject the completion)
     * and provide feedback that gets injected as a user message.
     *
     * @param summary The entropic.complete summary text.
     * @param ctx Loop context with tool results.
     * @return true if hook cancelled (completion rejected).
     * @version 2.0.10
     */
    bool fire_complete_hook(const std::string& summary,
                            const LoopContext& ctx);                ///< @internal

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
     * @param ctx Parent loop context that initiated the delegation.
     * @param tier Target tier the delegation is going to.
     * @param task Delegated task description.
     * @version 1.8.6
     */
    void fire_delegation_start(const LoopContext& ctx,
                               const std::string& tier,
                               const std::string& task);      ///< @internal

    /**
     * @brief Fire on_delegation_complete callback.
     * @param ctx Parent loop context that received the delegation result.
     * @param tier Target tier the delegation ran on.
     * @param result Final delegation result returned to the parent.
     * @version 1.8.6
     */
    void fire_delegation_complete(const LoopContext& ctx,
                                  const std::string& tier,
                                  const struct DelegationResult& result); ///< @internal

    // ── Members ──────────────────────────────────────────
    LoopMetrics last_metrics_;                           ///< P2-15: last run metrics
    std::unordered_map<std::string, LoopMetrics>
        per_tier_metrics_;                               ///< P2-15: per-tier accumulator (2.0.6-rc16.2)
    InferenceInterface inference_;                       ///< Inference contract
    LoopConfig loop_config_;                             ///< Loop config
    EngineCallbacks callbacks_;                          ///< Event callbacks
    std::atomic<bool> interrupt_flag_{false};             ///< Hard interrupt
    std::atomic<bool> pause_flag_{false};                 ///< Pause signal
    void (*external_interrupt_cb_)(void*) = nullptr;      ///< P1-10 transport abort
    void* external_interrupt_data_ = nullptr;             ///< Forwarded to cb
    char* (*validation_provider_)(void*) = nullptr;       ///< E3: ON_COMPLETE validation JSON
    void* validation_provider_data_ = nullptr;            ///< Forwarded to provider
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

    // ── Conversation state (v2.0.2) ─────────────────────────
    std::vector<Message> conversation_;                    ///< Persistent conversation
    std::string system_prompt_;                            ///< Cached system prompt
    SessionLogger* session_logger_ = nullptr;              ///< Non-owning model log

    // ── Pre-resolved tier data (v2.0.2) ─────────────────────
    std::unordered_map<std::string, ChildContextInfo> tier_info_;  ///< Tier → context info
    std::unordered_map<std::string, std::vector<std::string>> handoff_rules_; ///< Tier → targets
    /// @brief Tiers that relay single-delegate results verbatim (v2.0.11).
    std::unordered_set<std::string> relay_single_delegate_tiers_;

    /**
     * @brief Wire internal TierResolutionInterface from stored tier data.
     * @internal
     * @version 2.0.2
     */
    void wire_internal_tier_resolution();

    /**
     * @brief Apply per-identity overrides to effective limits in the context.
     *
     * Reads max_iterations and max_tool_calls_per_turn from the tier
     * resolution interface and stores them in ctx for use by should_stop
     * and ToolExecutor::truncate_to_limit. No-op if locked_tier is empty.
     *
     * @param ctx Loop context to update.
     * @internal
     * @version 2.0.6-rc16
     */
    void apply_identity_overrides(LoopContext& ctx);

    /**
     * @brief Resolve effective max_iterations, honouring per-identity override.
     * @param ctx Loop context.
     * @return Override if set (>=0), otherwise LoopConfig default.
     * @internal
     * @version 2.0.6-rc16
     */
    int resolve_max_iterations(const LoopContext& ctx) const;

    /**
     * @brief Resolve effective max_tool_calls_per_turn, honouring override.
     * @param ctx Loop context.
     * @return Override if set (>=0), otherwise LoopConfig default.
     * @internal
     * @version 2.0.6-rc16
     */
    int resolve_max_tool_calls(const LoopContext& ctx) const;

    /* ── TierResolutionInterface trampolines (static — accept void* ud) ── */
    /**
     * @brief Resolve tier (TierResolutionInterface trampoline).
     * @param name Tier name.
     * @param ud Untyped AgentEngine* pointer.
     * @return ChildContextInfo (valid=false if tier unknown).
     * @internal
     * @version 2.0.2
     */
    static ChildContextInfo tri_resolve_tier(
        const std::string& name, void* ud);
    /**
     * @brief Check tier existence (TierResolutionInterface trampoline).
     * @param name Tier name.
     * @param ud Untyped AgentEngine* pointer.
     * @return true if tier registered.
     * @internal
     * @version 2.0.2
     */
    static bool tri_tier_exists(
        const std::string& name, void* ud);
    /**
     * @brief Look up handoff targets (TierResolutionInterface trampoline).
     * @param name Source tier name.
     * @param ud Untyped AgentEngine* pointer.
     * @return Target tiers, empty if no handoff configured.
     * @internal
     * @version 2.0.2
     */
    static std::vector<std::string> tri_get_handoff_targets(
        const std::string& name, void* ud);
    /**
     * @brief Look up named tier parameter (TierResolutionInterface trampoline).
     * @param name Tier name.
     * @param param Parameter key.
     * @param ud Untyped AgentEngine* pointer.
     * @return String value, empty if tier or param unknown.
     * @internal
     * @version 2.0.2
     */
    static std::string tri_get_tier_param(
        const std::string& name, const std::string& param, void* ud);
};

} // namespace entropic
