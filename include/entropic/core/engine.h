// SPDX-License-Identifier: Apache-2.0
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

#include <entropic/entropic.h>  // for ent_decision_t and ent_delegation_* (gh#29, v2.1.5)
#include <entropic/core/compaction.h>
#include <entropic/core/context_manager.h>
#include <entropic/core/directives.h>
#include <entropic/core/conversation_state.h>
#include <entropic/core/engine_types.h>
#include <entropic/core/response_generator.h>
#include <entropic/core/sandbox.h>  // gh#33 (v2.1.6): engine-owned session sandbox
#include <entropic/interfaces/i_hook_handler.h>
#include <entropic/core/stream_think_filter.h>
#include <entropic/interfaces/i_inference_callbacks.h>
#include <entropic/types/session_logger.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>  // gh#32 (v2.1.6) resume payload type

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
     * @brief Grouped consumer-registered delegation callbacks (gh#29).
     *
     * Public so `delegation_callbacks_snapshot()` can return it. The
     * struct mirrors the C ABI surface: a start gate, a complete
     * delivery, and a shared user_data pointer. Snapshotted under
     * `delegation_cb_mutex_` to avoid torn reads.
     *
     * @version 2.1.5
     */
    struct DelegationCallbacks {
        ent_decision_t (*start)(const ent_delegation_request_t*, void*)
            = nullptr;
        ent_decision_t (*complete)(const ent_delegation_result_t*, void*)
            = nullptr;
        void* user_data = nullptr;
    };

    /**
     * @brief Run the engine on a set of messages.
     * @param messages Initial messages (system + user).
     * @param tier_override gh#99: if non-empty, lock this run to the named
     *        tier (skips routing) so its grammar/samplers apply; empty routes.
     * @return Final messages including all generated content.
     * @version 2.8.0
     */
    std::vector<Message> run(std::vector<Message> messages,
                             const std::string& tier_override = "");

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
     * @brief Set the configured project directory (gh#31, v2.1.6).
     *
     * The facade calls this from `entropic_configure_dir` after the
     * layered config loader has resolved the project root. `get_repo_dir()`
     * uses this value in preference to `std::filesystem::current_path()`,
     * so consumers whose launcher cwd differs from the target project
     * (the common case for wrapper CLIs and IDE plugins) still snapshot
     * the right tree into the sandbox.
     *
     * Pre-2.1.6 the engine used CWD unconditionally, silently ignoring
     * the `project_dir` argument to `entropic_configure_dir`. Setting
     * an empty path resets the override so `get_repo_dir()` falls back
     * to CWD (preserves the no-configure-dir caller's behavior).
     *
     * @param project_dir Project root (empty resets to CWD fallback).
     * @threadsafety Serialized per-handle via the facade's api_mutex.
     * @version 2.1.6
     */
    void set_project_dir(const std::filesystem::path& project_dir);

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
     * @brief Register delegation start/complete callbacks (gh#29, v2.1.5).
     *
     * Replaces the pre-2.1.5 silent auto-merge-to-parent behavior. The
     * engine stores these C-style function pointers and forwards them
     * into every `DelegationManager` it constructs (one per pending
     * delegation or pipeline). Null callbacks are honored: `on_start`
     * null = always ACCEPT; `on_complete` null = default-deny (write
     * patch to `~/.entropic/sandbox/<session>/pending/<id>.patch`).
     *
     * @param on_start    Pre-delegation gate (nullable clears).
     * @param on_complete Post-delegation result (nullable clears).
     * @param user_data   Forwarded to both callbacks.
     * @version 2.1.5
     */
    void set_delegation_callbacks(
        ent_decision_t (*on_start)(const ent_delegation_request_t*, void*),
        ent_decision_t (*on_complete)(const ent_delegation_result_t*, void*),
        void* user_data);

    /**
     * @brief Atomically snapshot the registered delegation callbacks.
     *
     * Hot path is `execute_pending_delegation` / `execute_pending_pipeline`:
     * they grab the triple under the mutex once and pass it forward to
     * the per-call DelegationManager. Prevents a torn read where the
     * consumer reassigns the callbacks mid-delegation. (Hardening
     * landed alongside the 2.1.5 verification pass.)
     *
     * @return Copy of the current callback struct.
     * @utility
     * @version 2.1.5
     */
    DelegationCallbacks delegation_callbacks_snapshot() const;

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
     * @brief Register the external transport interrupt-release callback.
     *
     * gh#150: the counterpart to set_external_interrupt(). The facade
     * wires this to ServerManager::clear_external_tool_interrupts so
     * reset_interrupt() releases the transports it interrupted. Without
     * it an interrupt was permanent, because the engine cleared only its
     * own flag.
     *
     * @param cb Callback (nullable).
     * @param user_data Forwarded to cb.
     * @req REQ-LOOP-006
     * @version 2.12.1
     */
    void set_external_reset(void (*cb)(void* user_data),
                            void* user_data);

    /**
     * @brief Run the engine loop on a pre-built context.
     *
     * Used by DelegationManager for child loops. Public so the
     * delegation manager (same .so) can invoke it.
     *
     * gh#81 (v2.4.3): `inherit_interrupt` controls whether the
     * pre-existing interrupt flag is cleared at entry. Top-level
     * turns clear it (a fresh turn starts un-interrupted). Delegation
     * child loops pass `inherit_interrupt=true` so a parent interrupt
     * raised before/at dispatch is NOT cleared — "stop the engine"
     * must stop children too, not reset on each nested loop.
     *
     * @param ctx Loop context to execute.
     * @param inherit_interrupt When true, do not reset the interrupt
     *        flag at entry (child loops inherit the parent's state).
     * @version 2.4.3
     */
    void run_loop(LoopContext& ctx, bool inherit_interrupt = false);

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
     * @brief Run a single turn under a named tier's grammar/identity (gh#99).
     *
     * Per-call tier selection on the shared resident model: seeds the tier's
     * system prompt (from `tier_info_`) and locks the run to `tier` so its
     * grammar + samplers apply — no 2nd model load, no reconfigure. Mirrors
     * the delegation precedent (a child LoopContext with locked_tier + the
     * tier's system prompt), scoped to one top-level call.
     *
     * @param tier Tier name to run this call under.
     * @param input User input string.
     * @return Full result messages from the engine.
     * @version 2.8.0
     */
    std::vector<Message> run_turn_as(const std::string& tier,
                                     const std::string& input);

    /**
     * @brief Run a single conversation turn from a pre-built message list (gh#37).
     *
     * Multimodal-aware overload. Each message in `new_messages` is
     * appended to the engine's conversation history with its
     * `content_parts` preserved (no string flattening). The agentic
     * loop runs over the full conversation; replies are appended to
     * history and returned.
     *
     * If the conversation is currently empty AND none of
     * `new_messages` carries `role == "system"`, the engine prepends
     * its configured `system_prompt_` first — matching the
     * single-string overload's behavior.
     *
     * @param new_messages Messages to add this turn (typically one
     *        user message; may include a system message on first
     *        turn).
     * @return Full result messages from the engine loop.
     * @version 2.1.8
     */
    std::vector<Message> run_turn(std::vector<Message> new_messages);

    /**
     * @brief Prepend the configured system prompt if this turn needs it.
     *
     * Extracted from run_turn to keep it knots-clean. Seeds
     * conversation_ with system_prompt_ only when the conversation is
     * empty and the caller didn't supply its own system message.
     *
     * @param new_messages The messages the caller is adding this turn.
     * @dg_internal
     * @version 2.3.7
     */
    void seed_system_prompt(const std::vector<Message>& new_messages);

    /**
     * @brief Shared drain loop for run_turn / run_turn_as (gh#99).
     *
     * Appends `pending` as a user turn, runs the agentic loop under
     * `tier_override` ("" = route), then drains any mid-generation queued
     * user messages as further turns. Extracted so both entry points stay
     * under the SLOC gate.
     *
     * @param pending First user turn for this call.
     * @param tier_override Tier to lock the run to ("" = route).
     * @return Result messages from the final turn.
     * @dg_internal
     * @version 2.8.0
     */
    std::vector<Message> run_drain_loop(std::string pending,
                                        const std::string& tier_override);

    /**
     * @brief Seed a tier's system prompt on an empty conversation (gh#99).
     *
     * Pushes `tier_info_[tier].system_prompt` as the system message when the
     * conversation is empty; falls back to the global `system_prompt_` when
     * the tier carries no info.
     *
     * @param tier Tier whose system prompt to seed.
     * @dg_internal
     * @version 2.8.0
     */
    void seed_system_prompt_for_tier(const std::string& tier);

    /**
     * @brief Pull the next queued user message into `pending` (gh#40).
     *
     * Extracted from run_turn's drain loop. Pops one queued message,
     * fires ON_QUEUE_CONSUMED, and replaces `pending` with it.
     *
     * @param pending Out: cleared and set to the next user turn.
     * @return true if a queued message was dequeued; false if empty.
     * @dg_internal
     * @version 2.3.7
     */
    bool prepare_next_turn(std::vector<Message>& pending);

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
     * @brief Streaming conversation turn from a pre-built message list (gh#37).
     *
     * Multimodal-aware streaming overload. Same content_parts
     * preservation semantics as run_turn(vector<Message>), with
     * token callback wiring identical to the single-string streaming
     * variant.
     *
     * @param new_messages Messages to add this turn.
     * @param on_token Consumer token callback (filtered UTF-8).
     * @param user_data Consumer callback context.
     * @param cancel_flag Polled per-token, nullable.
     * @return 0 on success, 1 if cancelled, 2 on error.
     * @version 2.1.8
     */
    int run_streaming(std::vector<Message> new_messages,
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

    // ── Mid-generation user-message queue (gh#40, v2.1.10) ──

    /**
     * @brief Append a user message to the mid-gen queue.
     *
     * Bounded FIFO. Enforces `LoopConfig::message_queue_capacity`.
     * Drained between top-level `run_turn` invocations from inside
     * `run_turn` itself — never at child-delegation boundaries.
     *
     * @param message User input to enqueue.
     * @return true if enqueued; false if the queue is at capacity.
     * @threadsafety Thread-safe.
     * @version 2.1.10
     */
    bool queue_user_message(const std::string& message);

    /**
     * @brief Snapshot of the queue depth.
     * @threadsafety Thread-safe.
     * @version 2.1.10
     */
    size_t user_message_queue_depth() const;

    /**
     * @brief Drop all queued user messages.
     * @threadsafety Thread-safe.
     * @version 2.1.10
     */
    void clear_user_message_queue();

    /**
     * @brief Set the runtime capacity of the mid-gen queue.
     *
     * If `cap` is less than the current depth, excess pending
     * messages are kept (no truncation) but no new enqueues succeed
     * until the queue drains below the new cap. `cap` of 0 disables
     * enqueue while leaving the queue otherwise functional.
     *
     * @param cap New capacity.
     * @threadsafety Thread-safe.
     * @version 2.1.10
     */
    void set_message_queue_capacity(int cap);

    /**
     * @brief Whether a top-level run_turn is currently in progress.
     *
     * Used by the facade to reject `entropic_queue_user_message` with
     * `ENTROPIC_ERROR_INVALID_STATE` when there is no in-flight turn
     * to "queue behind." Thread-safe (atomic load).
     *
     * @return true while a top-level run_turn is executing.
     * @utility
     * @version 2.1.10
     */
    bool is_running() const { return running_flag_.load(); }

    /**
     * @brief Try to claim one turn for a named session (gh#144 / gh#158).
     *
     * @par What the claim costs
     * gh#144 made it a compare-exchange on `running_flag_` — deliberately NOT
     * a mutex, because gh#109 removed `api_mutex` from every run entry point
     * so a long turn could not block `entropic_interrupt()` from another
     * thread. gh#158 keys the claim, which needs a map, which needs a lock —
     * but `runs_mutex_` is held only for the O(1) insert and is NEVER held
     * across a decode, a prefill or a tool call. That is the property gh#109
     * actually protects; a lock held for nanoseconds does not violate it,
     * whereas `api_mutex` (held for the whole turn) did.
     *
     * @par What the key changes
     * Two runs on the SAME key are always a genuine conflict — they share one
     * conversation — so the second is refused whatever the configuration.
     * Two runs on DIFFERENT keys proceed together only when
     * `set_concurrent_sessions(true)` has been called; off (the shipped
     * default) the guard stays handle-exclusive and v2.12.0 semantics hold
     * byte for byte.
     *
     * @param key Session key to claim; `""` is the default session.
     * @return true when this caller now owns the turn and MUST call
     *         `end_turn(key)`; false when the claim is refused, in which case
     *         the caller owns nothing and must not release.
     * @req REQ-API-009
     * @req REQ-LOOP-009
     * @version 2.13.0
     */
    bool try_begin_turn(const std::string& key);

    /**
     * @brief Claim a turn for the session bound to THIS thread (gh#158).
     *
     * The no-argument form every pre-gh#158 call site already uses. It
     * resolves to whatever `set_active_session` published on this thread, so
     * `run_turn`'s internal "claim only if the caller has not already" probe
     * asks about the right session rather than always about `""`.
     *
     * @return As `try_begin_turn(key)`.
     * @req REQ-API-009
     * @version 2.13.0
     */
    bool try_begin_turn() { return try_begin_turn(active_session_key()); }

    /**
     * @brief Release a turn claimed by try_begin_turn (gh#144 / gh#158).
     *
     * Only the caller whose `try_begin_turn` returned true may call this.
     * @param key Session key that was claimed.
     * @req REQ-API-009
     * @version 2.13.0
     */
    void end_turn(const std::string& key);

    /**
     * @brief Release the turn this thread claimed (gh#158).
     *
     * Releases by the key recorded at claim time, so a caller that claimed
     * `"A"` cannot accidentally release `""` — which is what an unkeyed
     * release would have done for every pre-gh#158 call site.
     *
     * @req REQ-API-009
     * @version 2.13.0
     */
    void end_turn();

    /**
     * @brief Number of runs currently in flight on this handle (gh#158).
     * @return Active run count; 0 when idle.
     * @utility
     * @version 2.13.0
     */
    std::size_t active_run_count() const;

    /**
     * @brief Allow runs on DIFFERENT session keys to proceed together.
     *
     * @par Why this is opt-in and defaults off
     * Per-key runs are only safe once every piece of per-handle mutable state
     * a turn touches is either per-run or locked. That audit is recorded in
     * decision #66 of `docs/architecture-cpp.md`. Shipping the concurrency ON
     * by default would make every existing consumer concurrent without their
     * asking — the mistake gh#157 explicitly refused to repeat with
     * `keep_warm` — and a racy default is strictly worse than today's honest
     * serialization.
     *
     * Config key: `concurrent_sessions: true`.
     *
     * @param enabled true to allow concurrent distinct-key runs.
     * @req REQ-LOOP-009
     * @version 2.13.0
     */
    void set_concurrent_sessions(bool enabled);

    /**
     * @brief Whether per-session concurrency is enabled (gh#158).
     * @return true when distinct keys may run together.
     * @utility
     * @version 2.13.0
     */
    bool concurrent_sessions() const;

    /**
     * @brief Interrupt exactly one session's run (gh#158).
     *
     * `interrupt()` means "every run on this handle" and keeps that meaning.
     * This means "that one", and deliberately does NOT fire the external
     * transport latch: `ServerManager::interrupt_external_tools()` trips
     * EVERY transport at once, so using it here would abort another session's
     * in-flight MCP call — the gh#150 failure mode, re-introduced through a
     * different door. The interrupted run's own tool call still aborts,
     * because the run publishes its cancel token to its own thread
     * (`RunCancelScope`) and the transport polls it.
     *
     * @param key Session whose run to interrupt. No-op when it is not running.
     * @return true when a run was found and flagged.
     * @req REQ-LOOP-006
     * @version 2.13.0
     */
    bool interrupt_session(const std::string& key);

    /**
     * @brief Whether a named session's in-flight run carries an interrupt.
     * @param key Session key.
     * @return true when that run exists and is flagged.
     * @utility
     * @version 2.13.0
     */
    bool session_interrupted(const std::string& key) const;

    /**
     * @brief The cancel flag of the run executing on THIS thread (gh#158).
     *
     * Handed to paths that take a `std::atomic<bool>&` cancel token —
     * `entropic_run_batch` is the one that had a LOCAL flag nobody could
     * ever set, which is why it was uninterruptible.
     *
     * Falls back to the handle-wide interrupt flag when no run is claimed on
     * this thread, so the reference is always valid.
     *
     * @return Reference to this run's cancel flag.
     * @req REQ-LOOP-006
     * @version 2.13.0
     */
    std::atomic<bool>& run_cancel_flag();

    /**
     * @brief Bind this turn to a caller-scoped session (gh#144, v2.12.0).
     *
     * `""` is the default session and reproduces pre-2.12.0 behaviour
     * exactly. The key is OPAQUE to the engine — a consumer picks whatever
     * identifies a caller for it (a canonical repository path, a hash),
     * which is why two callers may deliberately share one by using the same
     * string.
     *
     * Distinct from `LoopContext::conversation_id`, which is a storage FK
     * minted fresh per run() and is neither caller-supplied nor stable, and
     * from the per-handle log scope. One word, one meaning.
     *
     * gh#158 (v2.13.0): the binding is published to the CALLING THREAD as
     * well as to the handle. Two concurrent runs each need their own answer
     * to "which session am I", and a single member field can only hold one.
     * The member remains the answer for a thread that never bound a session,
     * which is what keeps the legacy unkeyed accessors behaving as they did.
     *
     * @param key Session key for the turn about to run.
     * @req REQ-LOOP-001
     * @version 2.13.0
     */
    void set_active_session(const std::string& key);

    /**
     * @brief Session whose turn is currently running.
     * @return The active key; `""` when idle or unkeyed.
     * @utility
     * @version 2.12.0
     */
    const std::string& active_session_key() const;

    /**
     * @brief Messages held by a named session.
     *
     * Returns BY VALUE, unlike the legacy `get_messages()`. A keyed entry can
     * be erased by `drop_session`, and handing out a reference into an
     * erasable node is a use-after-free waiting for a later refactor. The
     * copy is a few thousand short strings on a path that runs once per turn,
     * against a turn measured in seconds.
     *
     * @param key Session key.
     * @return That session's messages; empty when the key is unknown.
     * @req REQ-LOOP-001
     * @version 2.12.0
     */
    std::vector<Message> messages_for(const std::string& key) const;

    /**
     * @brief Replace one session's conversation wholesale (gh#165, v2.13.0).
     *
     * The write counterpart `entropic_session_context_get` never had, so a
     * session could be read out but not restored: a host that stopped the
     * engine to free VRAM lost every conversation, and the only way to put
     * messages back — `entropic_run_session` — RUNS a turn per message,
     * re-executing the whole agentic loop including tool calls.
     *
     * Refuses the session that is CURRENTLY RUNNING. Every other session
     * stays mutable mid-run: the restriction is about the conversation this
     * turn is appending to, not about the handle being busy.
     *
     * @param key Session key; `""` is the default session.
     * @param messages Replacement conversation.
     * @return true when the session was replaced; false when a run on that
     *         key is in flight and the conversation was left untouched.
     * @req REQ-LOOP-001
     * @version 2.13.0
     */
    bool set_session_messages(const std::string& key,
                              std::vector<Message> messages);

    /**
     * @brief Message count for a named session.
     * @param key Session key.
     * @return Count, or 0 when the key is unknown.
     * @utility
     * @version 2.12.0
     */
    std::size_t message_count_for(const std::string& key) const;

    /**
     * @brief Clear one session's history, keeping the session itself.
     * @param key Session key.
     * @req REQ-LOOP-001
     * @version 2.12.0
     */
    void clear_conversation_for(const std::string& key);

    /**
     * @brief Forget a session entirely.
     *
     * The default session (`""`) is cleared rather than erased, so the legacy
     * reference-returning accessors stay total.
     *
     * @param key Session key.
     * @return true when a session was dropped or cleared.
     * @req REQ-LOOP-001
     * @version 2.12.0
     */
    bool drop_session(const std::string& key);

    /**
     * @brief Keys of every session the engine currently holds.
     * @return Session keys, unordered.
     * @utility
     * @version 2.12.0
     */
    std::vector<std::string> session_keys() const;

    /**
     * @brief Register an observer that fires when a queued user
     *        message is consumed and seeded as the next turn.
     *
     * Persistent across `set_callbacks()` reassignments — the
     * streaming entry points overwrite the EngineCallbacks struct
     * per-call, so the queue observer needs a dedicated slot to
     * survive both streaming and non-streaming runs.
     *
     * @param observer Callback (consumed_text, remaining_depth, ud).
     *        Pass nullptr to clear.
     * @param user_data Forwarded to observer.
     * @threadsafety Thread-safe.
     * @version 2.1.10
     */
    void set_queue_observer(
        void (*observer)(const char*, size_t, void*),
        void* user_data);

    /**
     * @brief Register a persistent state-transition observer.
     *
     * The legacy path (`EngineCallbacks::on_state_change`) is wiped
     * for the duration of `run_streaming` because that method
     * replaces the full callback struct to install its token
     * sink. This persistent slot survives every `set_callbacks()`
     * shuffle so consumers (notably the external MCP bridge that
     * the entropic_set_state_observer docstring names) actually see
     * transitions during streaming runs.
     *
     * Fires alongside the legacy `on_state_change` slot — neither
     * supersedes the other. Pass `nullptr` to clear.
     *
     * @param observer State-change callback.
     * @param user_data Forwarded to observer.
     * @threadsafety Thread-safe.
     * @version 2.1.10
     */
    void set_state_observer(
        void (*observer)(int state, void* user_data),
        void* user_data);

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
     * @brief Whether a tier name is known (gh#99).
     *
     * Backs entropic_run_as's unknown-tier validation. True once the tier
     * has resolved context info (set_tier_info ran for it at configure time).
     *
     * @param name Tier name.
     * @return true if the tier is known.
     * @version 2.8.0
     */
    bool has_tier(const std::string& name) const;

    /**
     * @brief Return the allowed tool names registered for a tier (gh#121 regression).
     *
     * Exposed so facade-level tests can verify that `populate_tier_info`
     * captured the correct `allowed_tools` list after `cache_tier_allowed_tools`
     * filled `h->tier_allowed_tools`. Returns empty when the tier is unknown
     * or when no allowed_tools were registered.
     *
     * @param name Tier name.
     * @return Allowed tool names (empty = unknown tier or all tools permitted).
     * @version 2.9.19
     */
    std::vector<std::string> get_tier_allowed_tools(
        const std::string& name) const;

    /**
     * @brief A tier's resolved system prompt (gh#98).
     *
     * Backs the batch entry point, which builds each request's messages with
     * its tier's system prompt so same-tier requests share a prompt prefix.
     *
     * @param name Tier name.
     * @return The tier's system prompt, or "" if the tier is unknown.
     * @version 2.8.0
     */
    const std::string& tier_system_prompt(const std::string& name) const;

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

    /**
     * @brief Predicate: should this delegation be blocked because the
     *        same target has just failed too many times? (gh#64)
     *
     * Returns true when `target` matches `ctx.last_failed_delegation_target`
     * and `ctx.consecutive_failed_delegations >=
     * loop_config_.max_consecutive_failed_delegations`. Exposed so
     * tests can exercise the guard without spinning a full child loop.
     *
     * @param ctx Active loop context.
     * @param target Proposed delegation target tier.
     * @return true if the delegation should be rejected before dispatch.
     * @utility
     * @version 2.3.0
     */
    bool is_delegation_repeat_blocked(
        const LoopContext& ctx, const std::string& target) const;

    /**
     * @brief gh#35: seconds since the engine last serviced a run().
     *
     * Updated at the entry of every run()/run_turn() call. Hosts that
     * want an idle-exit policy poll this and tear down the engine when
     * the value exceeds their threshold. Returned as int64 (epoch
     * seconds delta) so consumers can compare against any unit.
     *
     * @return Seconds since last activity. Returns 0 if no run has
     *         ever happened (no activity to be idle relative to).
     * @utility
     * @version 2.3.0
     */
    int64_t seconds_since_last_activity() const;

    /**
     * @brief gh#68 (v2.3.4): fold entropic.complete summary into the
     *        prior assistant message, suppressing the JSON tool-result
     *        user message that would otherwise land in history.
     *
     * Returns true when the message was folded (caller should NOT push
     * it). Returns false when the message should be pushed as-is.
     *
     * Conservative — only folds when ALL of:
     *   1. `msg.metadata["tool_name"] == "entropic.complete"`
     *   2. `ctx.messages.back()` exists and has `role == "assistant"`
     *   3. that assistant message has empty `content` (typically the
     *      post-strip body when the model emitted only a tool_call)
     *   4. `ctx.metadata["explicit_completion_summary"]` is populated
     *      (set by `dir_complete` before this runs)
     *
     * If the model also emitted prose around the tool_call, the
     * assistant body is non-empty and we leave both messages alone
     * to avoid silently overwriting legitimate text.
     *
     * Public so unit tests can exercise the predicate without
     * spinning the full engine loop + mocking the tool executor.
     *
     * @utility
     * @version 2.3.4
     */
    bool fold_complete_into_assistant(
        LoopContext& ctx, const Message& tool_result_msg) const;

    /**
     * @brief gh#88: reshape a meta-tool `{"action":...}` result so the
     *        model is not primed to parrot the call-shaped envelope.
     *
     * delegate / pipeline (and peers) return their result as
     * `{"action":"x",...}` JSON. Pushed verbatim into context, the model
     * echoes that shape as its next "tool call" — which common_chat
     * (PEG_GEMMA4) does not parse, so the call no-ops and the loop spirals
     * to the iteration cap. The typed directive is already built by
     * ToolExecutor::build_directive (target/task copied into the struct)
     * before this runs, so the model-facing content can be replaced with a
     * non-call prose status line without affecting dispatch.
     * `entropic.complete` is de-fanged earlier by the fold above.
     *
     * Public so unit tests can exercise the transform without spinning the
     * full engine loop.
     *
     * @param msg Tool-result message; content reshaped in place when it is
     *            an `entropic.*` meta-tool action envelope.
     * @utility
     * @version 2.7.1
     */
    void defang_meta_action_envelope(Message& msg) const;

    /**
     * @brief Borrowed reference to the engine's built-in CompactionManager.
     *
     * gh#76 (v2.3.27): the facade-level `CompactorRegistry` borrows
     * this reference at construction so consumer-registered compactors
     * can fall back to the built-in default. Lifetime: the
     * CompactionManager outlives the registry because both are owned
     * by the engine handle (registry is destroyed in `entropic_destroy`
     * before the engine).
     *
     * @utility
     * @version 2.3.27
     */
    CompactionManager& compaction_manager() { return compaction_manager_; }

private:
    /**
     * @brief Create the root conversation row for a run (gh#48).
     *
     * Extracted from run() to keep it knots-clean. Sets
     * ctx.conversation_id when storage is wired so delegations carry
     * a valid FK; logs a warning (non-fatal) on failure.
     *
     * @param ctx Loop context.
     * @dg_internal
     * @version 2.3.7
     */
    void init_session_conversation(LoopContext& ctx);

    /**
     * @brief Fold a finished run's metrics into the per-tier totals.
     *
     * Extracted from run() to keep it knots-clean. Snapshots
     * last_metrics_ and accumulates into per_tier_metrics_.
     *
     * @param ctx Loop context (with completed metrics).
     * @dg_internal
     * @version 2.3.7
     */
    void accumulate_run_metrics(LoopContext& ctx);

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
     * @brief Shared relay machinery: fire hook, write summary, set COMPLETE.
     * @param ctx Loop context.
     * @param summary Content to relay (caller applies any prefix).
     * @dg_internal
     * @version 2.1.0
     */
    void relay_partial_result(LoopContext& ctx, const std::string& summary);

    /**
     * @brief Emit disambiguating log + metadata for relay path.
     * @param ctx Loop context (metadata mutated).
     * @param terminal_reason Non-empty when relaying a budget_exhausted child.
     * @dg_internal
     * @version 2.1.0
     */
    void log_relay_status(LoopContext& ctx,
                          const std::string& terminal_reason = {});

    /**
     * @brief Check if loop should stop.
     * @param ctx Loop context.
     * @return true if loop should terminate.
     * @version 1.8.4
     */
    bool should_stop(const LoopContext& ctx) const;

    /**
     * @brief True when ctx.state is COMPLETE / ERROR / INTERRUPTED.
     *
     * The terminal-state predicate alone (no iteration / duplicate
     * limit). Distinct from should_stop, which folds in budget gates.
     * Callers that want to honor a terminal state set by a sibling
     * tool call (e.g. entropic.complete emitted alongside an action
     * tool) without also short-circuiting on budget use this.
     *
     * @param ctx Loop context.
     * @return true if state is one of the three terminals.
     * @utility
     * @version 2.3.28
     */
    static bool is_terminal_state(const LoopContext& ctx);

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
    void dir_stop(LoopContext&, const Directive&, DirectiveResult&);         ///< @dg_internal
    void dir_tier_change(LoopContext&, const Directive&, DirectiveResult&);  ///< @dg_internal
    void dir_delegate(LoopContext&, const Directive&, DirectiveResult&);     ///< @dg_internal
    void dir_pipeline(LoopContext&, const Directive&, DirectiveResult&);     ///< @dg_internal
    void dir_complete(LoopContext&, const Directive&, DirectiveResult&);     ///< @dg_internal
    void dir_clear_todos(LoopContext&, const Directive&, DirectiveResult&);  ///< @dg_internal
    void dir_inject(LoopContext&, const Directive&, DirectiveResult&);       ///< @dg_internal
    void dir_prune(LoopContext&, const Directive&, DirectiveResult&);        ///< @dg_internal
    void dir_anchor(LoopContext&, const Directive&, DirectiveResult&);       ///< @dg_internal
    void dir_phase(LoopContext&, const Directive&, DirectiveResult&);        ///< @dg_internal
    void dir_notify(LoopContext&, const Directive&, DirectiveResult&);       ///< @dg_internal

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
     * @return gh#84 (v2.5.1): true if at least one tool genuinely
     *         executed (result_kind ok / ok_empty), false if every
     *         call was rejected/errored — drives the thinking-budget
     *         reset so duplicate/rejected-spam can't refresh it.
     * @version 2.5.1
     */
    bool process_tool_results(LoopContext& ctx,
                              const std::vector<ToolCall>& tool_calls);

    // ── Delegation (v1.8.6) ──────────────────────────────

    /**
     * @brief Execute a pending delegation after tool processing.
     * @param ctx Loop context with pending_delegation set.
     * @version 1.8.6
     */
    void execute_pending_delegation(LoopContext& ctx);           ///< @dg_internal

    /**
     * @brief Apply the pre-run delegation guards (depth/cycle/repeat).
     *
     * Extracted from execute_pending_delegation to keep it knots-clean.
     * On rejection, logs and pushes the appropriate rejection message
     * into ctx, then reports true so the caller bails in one return.
     *
     * @param ctx Loop context.
     * @param pending The delegation about to run.
     * @return true if the delegation was rejected (already handled).
     * @dg_internal
     * @version 2.3.7
     */
    bool reject_delegation_if_guarded(LoopContext& ctx,
                                      const PendingDelegation& pending);

    /**
     * @brief Execute a pending pipeline after tool processing.
     * @param ctx Loop context with pending_pipeline set.
     * @version 1.8.6
     */
    void execute_pending_pipeline(LoopContext& ctx);             ///< @dg_internal

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
                           const std::string& content);         ///< @dg_internal

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
                        const std::string& content);            ///< @dg_internal

    /**
     * @brief Fire a pre-hook and return true if cancelled.
     * @param point Hook point.
     * @param iteration Current iteration number.
     * @return true if hook cancelled the operation.
     * @version 1.9.1
     */
    bool fire_pre_hook(entropic_hook_point_t point, int iteration); ///< @dg_internal

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
                                    const std::vector<Message>& messages); ///< @dg_internal

    /**
     * @brief Pull rejection text from validation_provider_ into
     *        ctx.pending_validation_feedback for the next turn.
     *
     * Queries the validation_provider_ callback (if wired); when
     * verdict starts with "rejected", joins violations into a
     * one-line summary stashed on the loop context. No-op when
     * validation_provider_ is not wired or the verdict is clean.
     *
     * @param ctx Loop context (mutates pending_validation_feedback).
     * @dg_internal
     * @version 2.1.0
     */
    void capture_validation_feedback(LoopContext& ctx);

    /**
     * @brief Run post-generate bookkeeping in one call.
     *
     * Per-iteration housekeeping bundled to keep
     * execute_iteration knots-clean:
     *   1. Clear ctx.pending_validation_feedback (just consumed by
     *      generate_response via inject_engine_state_reminder).
     *   2. Fire ENTROPIC_HOOK_POST_GENERATE (validator may revise
     *      result.content here).
     *   3. capture_validation_feedback(ctx) — stash next-turn
     *      feedback if the validator rejected.
     *
     * Demo ask #2 (v2.1.0).
     *
     * @param ctx Loop context.
     * @param result Generation result (content may be revised by
     *               POST_GENERATE hook).
     * @dg_internal
     * @version 2.1.0
     */
    void dispatch_post_generate(LoopContext& ctx,
                                GenerateResult& result);

    /**
     * @brief Turn a generation result into the next loop state.
     *
     * Post-generate processing extracted to keep execute_iteration
     * knots-clean: parse tool calls, append the assistant message,
     * dispatch tool results (or the no-tool decision), then run any
     * pending delegation/pipeline the turn produced.
     *
     * @param ctx Loop context.
     * @param result Generation result (already post-processed).
     * @dg_internal
     * @version 2.3.7
     */
    void process_generation_result(LoopContext& ctx,
                                   GenerateResult& result);

    /**
     * @brief Dispatch a queued delegation/pipeline, or halt the turn.
     *
     * Extracted from process_generation_result (gh#81/gh#77, v2.4.3).
     * Honors a mid-tool-processing interrupt and a sibling-set
     * terminal state before launching any queued action.
     *
     * @param ctx Loop context.
     * @dg_internal
     * @version 2.4.3
     */
    void dispatch_pending_or_halt(LoopContext& ctx);

    /**
     * @brief Charge the thinking budget for this iteration. (gh#80, v2.5.0)
     *
     * No-op when `loop_config_.budget_mode == off`. A tool call resets
     * the per-turn accumulator (productive action is free). Otherwise
     * the iteration's generation is charged (estimated tokens or
     * wall-clock seconds). On the first exhaustion the engine pushes a
     * "emit completion now" nudge into history; a second exhaustion
     * without an intervening tool call hard-cuts the turn with a
     * failure note (also visible in history) and sets COMPLETE.
     *
     * @param ctx Loop context (accumulator + state mutated).
     * @param content_len Byte length of this iteration's generated
     *        content (used to estimate tokens for the `tokens` mode).
     * @param made_tool_call Whether this iteration dispatched a tool.
     * @dg_internal
     * @version 2.5.0
     */
    void charge_thinking_budget(LoopContext& ctx, size_t content_len,
                                bool made_tool_call);

    /**
     * @brief Accumulate + return budget units consumed this window.
     * @param ctx Loop context (accumulator mutated).
     * @param content_len Generated content byte length (tokens mode).
     * @return Units consumed (estimated tokens, or wall-clock seconds).
     * @dg_internal
     * @version 2.5.0
     */
    int budget_units_consumed(LoopContext& ctx, size_t content_len);

    /**
     * @brief First-exhaustion budget nudge (push "emit completion now").
     * @param ctx Loop context.
     * @dg_internal
     * @version 2.5.0
     */
    void nudge_budget_completion(LoopContext& ctx);

    /**
     * @brief Second-exhaustion hard cut (failure note + terminal).
     * @param ctx Loop context.
     * @dg_internal
     * @version 2.5.0
     */
    void hard_cut_budget(LoopContext& ctx);

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
                            const LoopContext& ctx);                ///< @dg_internal

    /**
     * @brief Fire ON_DELEGATE pre-hook. Returns true if cancelled.
     * @param pending Delegation info.
     * @param depth Current delegation depth.
     * @return true if hook cancelled delegation.
     * @version 1.9.1
     */
    bool fire_delegate_pre_hook(const PendingDelegation& pending,
                                int depth);                         ///< @dg_internal

    /**
     * @brief Fire ON_DELEGATE_COMPLETE post-hook.
     *
     * Issue #7 (v2.1.4): JSON now includes typed `result_kind` and
     * `summary` so consumers don't have to content-prefix-match.
     *
     * @param target Target tier.
     * @param success Whether delegation succeeded.
     * @param summary Child summary or terminal_reason (verbatim; empty OK).
     * @version 2.1.4
     */
    void fire_delegate_complete_hook(const std::string& target,
                                     bool success,
                                     const std::string& summary = "");///< @dg_internal

    /**
     * @brief Get the project root used as sandbox snapshot source.
     *
     * Preference order (gh#31, v2.1.6):
     *   1. The path stored via `set_project_dir()` (populated by
     *      `entropic_configure_dir`).
     *   2. `std::filesystem::current_path()` as a fallback when no
     *      project_dir was configured (preserves legacy facade callers).
     *
     * Cached on first call. Unlike the v1.8.6–v2.1.4 implementation, this
     * method does NOT initialize a git repo if none exists — `SandboxManager`
     * handles non-git projects natively (gh#29, v2.1.5). The engine never
     * mutates the user's project directory.
     *
     * @return Project directory path.
     * @version 2.1.6
     */
    std::filesystem::path get_repo_dir();                       ///< @dg_internal

    /**
     * @brief Lazily construct (or return) the session-scoped SandboxManager.
     *
     * gh#33 (v2.1.6): pre-2.1.6 each delegation built a fresh
     * `DelegationManager` as a stack local, which owned a fresh
     * `SandboxManager` and dropped it on return — re-snapshotting the
     * entire project on every delegation and emitting a misleading
     * "Session sandbox cleanup" log after every call. The manager is
     * now engine-scoped: created once on first delegation, destroyed
     * when the engine is destroyed.
     *
     * Returns nullptr when `get_repo_dir()` resolves to an empty path
     * (no project configured) — callers fall back to the no-sandbox
     * child-loop path, matching pre-2.1.6 behavior for that case.
     *
     * @return Pointer to the engine-scoped sandbox manager, or nullptr
     *         when no project_dir is available.
     * @threadsafety Construction is serialized by the facade's api_mutex.
     * @version 2.1.6
     */
    SandboxManager* ensure_sandbox_manager();                   ///< @dg_internal

    /**
     * @brief Resolve a resume_delegation pending request against storage.
     *
     * gh#32 (v2.1.6). Calls `storage_.load_delegation_with_messages`,
     * parses the result, and populates `pending.target` (from the loaded
     * `target_tier`) and `out_history` (from the loaded `messages`).
     * On failure (no storage, unknown id, parse error) writes a typed
     * `[DELEGATION FAILED: resume_delegation ...]` user message to the
     * parent context and returns false; the caller bails out.
     *
     * @param ctx          Parent loop context (failure message lands here).
     * @param pending      Pending delegation (target rewritten on success).
     * @param out_history  Loaded conversation messages on success.
     * @return true on success.
     * @dg_internal
     * @version 2.1.6
     */
    bool resolve_resume_delegation(
        LoopContext& ctx,
        PendingDelegation& pending,
        std::vector<Message>& out_history);

    /**
     * @brief Helper for `resolve_resume_delegation` (gh#32, v2.1.6).
     *
     * Calls the storage callback, parses the JSON payload, pushes a
     * typed failure message to `ctx` on any error path. Extracted so
     * the parent function stays under the knots returns/SLOC gates.
     *
     * @param ctx     Parent context (receives failure message on error).
     * @param id      Delegation id.
     * @param parsed  [out] Parsed JSON payload on success.
     * @return true on success.
     * @dg_internal
     * @version 2.1.6
     */
    bool fetch_resume_payload(
        LoopContext& ctx,
        const std::string& id,
        nlohmann::json& parsed);

    /**
     * @brief Run a pending delegation (cold or resume).
     *
     * Extracted to keep `execute_pending_delegation` under the knots
     * SLOC gate. Builds a per-delegation `DelegationManager` against
     * the engine-scoped sandbox + storage interfaces and dispatches
     * to either `execute_delegation` or `execute_resume_delegation`
     * based on whether `resume_history` is empty.
     *
     * @param ctx Parent loop context (informational).
     * @param pending Pending delegation request.
     * @param resume_history Pre-loaded history (empty for cold delegations).
     * @return DelegationResult from the child loop.
     * @dg_internal
     * @version 2.1.6
     */
    DelegationResult run_pending_delegation(
        LoopContext& ctx,
        const PendingDelegation& pending,
        std::vector<Message> resume_history);

    /**
     * @brief Fire on_delegation_start callback.
     * @param ctx Parent loop context that initiated the delegation.
     * @param tier Target tier the delegation is going to.
     * @param task Delegated task description.
     * @version 1.8.6
     */
    void fire_delegation_start(const LoopContext& ctx,
                               const std::string& tier,
                               const std::string& task);      ///< @dg_internal

    /**
     * @brief Fire on_delegation_complete callback.
     * @param ctx Parent loop context that received the delegation result.
     * @param tier Target tier the delegation ran on.
     * @param result Final delegation result returned to the parent.
     * @version 1.8.6
     */
    void fire_delegation_complete(const LoopContext& ctx,
                                  const std::string& tier,
                                  const struct DelegationResult& result); ///< @dg_internal

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
    void (*external_reset_cb_)(void*) = nullptr;          ///< gh#150 transport un-abort
    void* external_reset_data_ = nullptr;                 ///< Forwarded to reset cb
    // ── Delegation callbacks (gh#29, v2.1.5) ────────────────
    /// @brief Held under `delegation_cb_mutex_` so set + snapshot
    /// can atomically swap all three fields without tearing. Bundled
    /// to prevent a race where a consumer reassigns callbacks while a
    /// delegation is in flight.
    DelegationCallbacks delegation_cb_;
    mutable std::mutex delegation_cb_mutex_;
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
    std::filesystem::path project_dir_override_;           ///< gh#31 (v2.1.6): set by configure_dir
    std::optional<SandboxManager> sandbox_mgr_;            ///< gh#33 (v2.1.6): session-scoped

    // ── Mid-generation user-message queue (gh#40, v2.1.10) ──
    mutable std::mutex queue_mutex_;          ///< Guards user_message_queue_
    std::deque<std::string> user_message_queue_; ///< FIFO mid-gen queue
    std::atomic<bool> running_flag_{false};   ///< Top-level run_turn in progress
    /// @brief gh#35 (v2.3.0): epoch-seconds timestamp of the most
    /// recent run()/run_turn() entry. Zero until the first run.
    std::atomic<int64_t> last_activity_epoch_s_{0};
    void (*queue_observer_)(const char*, size_t, void*) = nullptr; ///< gh#40 callback
    void* queue_observer_data_ = nullptr;     ///< Forwarded to queue_observer_
    /// @brief Persistent state-transition observer slot. Survives
    /// EngineCallbacks shuffles done by run_streaming. (gh#40 fallout)
    void (*state_observer_)(int, void*) = nullptr;
    void* state_observer_data_ = nullptr;     ///< Forwarded to state_observer_
    /**
     * @brief Pop one queued message if present.
     *
     * Used by run_turn to drain the queue at top-level COMPLETE.
     * Returns std::nullopt when the queue is empty.
     *
     * @threadsafety Thread-safe.
     * @dg_internal
     * @version 2.1.10
     */
    std::optional<std::string> pop_queued_user_message();

    /**
     * @brief Fire on_queued_message_consumed callback (gh#40).
     * @param consumed The popped message.
     * @param remaining Queue depth after the pop.
     * @dg_internal
     * @version 2.1.10
     */
    void fire_queue_consumed(const std::string& consumed, size_t remaining);

    // ── Conversation state (v2.0.2) ─────────────────────────
    /**
     * @brief The conversation this turn belongs to (gh#144, v2.12.0).
     * @return The active session's state.
     * @version 2.12.0
     */
    ConversationState& active_conversation();

    /// @brief gh#144 (v2.12.0): caller-scoped conversations, keyed.
    ///
    /// Replaces the single `conversation_` vector. `unordered_map` is chosen
    /// for its reference/pointer STABILITY across rehash (only iterators are
    /// invalidated): `get_messages()` returns a reference into the mapped
    /// value and every existing caller depends on that, so a container that
    /// relocates its elements would turn a working accessor into a dangling
    /// one.
    ///
    /// The `""` entry is constructed once and never erased, so the legacy
    /// accessors are total — they never have to invent an empty vector.
    std::unordered_map<std::string, ConversationState> conversations_;

    /// @brief gh#165 (v2.13.0): guards `conversations_` as a CONTAINER.
    ///
    /// The session APIs take `api_mutex` but not the run guard, so
    /// `entropic_session_context_clear` / `_drop` could already mutate this
    /// map while a turn appended to it — a latent race since v2.12.0 that
    /// keying runs per session turns into a likely one. Readers copy under
    /// it; `set_session_messages` / `drop_session` mutate under it.
    ///
    /// Held for map operations ONLY, never across a decode: `run_turn` takes
    /// a reference to one mapped value and works outside the lock, which is
    /// safe because `unordered_map` guarantees reference stability and the
    /// running key cannot be erased or replaced (both refuse it).
    mutable std::mutex conversations_mutex_;

    /**
     * @brief State private to ONE in-flight run (gh#158, v2.13.0).
     *
     * Every field here used to be one per HANDLE, which is exactly why a
     * handle could only run one turn: `interrupt()` clearing a shared flag at
     * the start of run B would have cleared run A's interrupt.
     *
     * Held by `shared_ptr` so a poller that captured the run (the cancel
     * reference handed to `entropic_run_batch`, the token published to the
     * run's thread) cannot outlive its storage when the run ends.
     *
     * @version 2.13.0
     */
    struct RunState {
        std::atomic<bool> interrupt{false};  ///< This run's cancel token
        std::string key;                     ///< Session this run owns
    };

    /// @brief gh#158: guards `active_runs_` and `concurrent_sessions_`.
    ///
    /// Held for O(1) map operations only — NEVER across a decode, a prefill
    /// or a tool call. That distinction is what keeps gh#109's property
    /// (a long turn must not block `entropic_interrupt()`) true.
    mutable std::mutex runs_mutex_;

    /// @brief gh#158: the runs currently in flight, keyed by session.
    std::unordered_map<std::string, std::shared_ptr<RunState>> active_runs_;

    /// @brief gh#158: opt-in — may DIFFERENT keys run together? Default no.
    bool concurrent_sessions_ = false;

    /**
     * @brief Resolve the run this thread owns, if any (gh#158).
     * @return This thread's run state, or nullptr outside a claimed run.
     * @dg_internal
     * @version 2.13.0
     */
    std::shared_ptr<RunState> this_thread_run() const;

    /**
     * @brief Whether the run on THIS thread should stop (gh#158).
     * @return true when the handle-wide flag or this run's token is set.
     * @dg_internal
     * @version 2.13.0
     */
    bool run_interrupted() const;

    /// @brief Session whose turn is currently running (gh#144).
    ///
    /// Set for the duration of a turn. Read by the StateProvider bridge so a
    /// tool inspecting context during session B's turn sees session B — no
    /// vtable change needed, because `get_history` is only ever invoked from
    /// inside a tool call, i.e. inside a turn.
    std::string active_session_key_;
    std::string system_prompt_;                            ///< Cached system prompt
    SessionLogger* session_logger_ = nullptr;              ///< Non-owning model log

    // ── Pre-resolved tier data (v2.0.2) ─────────────────────
    std::unordered_map<std::string, ChildContextInfo> tier_info_;  ///< Tier → context info
    std::unordered_map<std::string, std::vector<std::string>> handoff_rules_; ///< Tier → targets
    /// @brief Tiers that relay single-delegate results verbatim (v2.0.11).
    std::unordered_set<std::string> relay_single_delegate_tiers_;

    /**
     * @brief Wire internal TierResolutionInterface from stored tier data.
     * @dg_internal
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
     * @dg_internal
     * @version 2.0.6-rc16
     */
    void apply_identity_overrides(LoopContext& ctx);

    /**
     * @brief Resolve effective max_iterations, honouring per-identity override.
     * @param ctx Loop context.
     * @return Override if set (>=0), otherwise LoopConfig default.
     * @dg_internal
     * @version 2.0.6-rc16
     */
    int resolve_max_iterations(const LoopContext& ctx) const;

    /**
     * @brief Resolve effective max_tool_calls_per_turn, honouring override.
     * @param ctx Loop context.
     * @return Override if set (>=0), otherwise LoopConfig default.
     * @dg_internal
     * @version 2.0.6-rc16
     */
    int resolve_max_tool_calls(const LoopContext& ctx) const;

    /* ── TierResolutionInterface trampolines (static — accept void* ud) ── */
    /**
     * @brief Resolve tier (TierResolutionInterface trampoline).
     * @param name Tier name.
     * @param ud Untyped AgentEngine* pointer.
     * @return ChildContextInfo (valid=false if tier unknown).
     * @dg_internal
     * @version 2.0.2
     */
    static ChildContextInfo tri_resolve_tier(
        const std::string& name, void* ud);
    /**
     * @brief Check tier existence (TierResolutionInterface trampoline).
     * @param name Tier name.
     * @param ud Untyped AgentEngine* pointer.
     * @return true if tier registered.
     * @dg_internal
     * @version 2.0.2
     */
    static bool tri_tier_exists(
        const std::string& name, void* ud);
    /**
     * @brief Look up handoff targets (TierResolutionInterface trampoline).
     * @param name Source tier name.
     * @param ud Untyped AgentEngine* pointer.
     * @return Target tiers, empty if no handoff configured.
     * @dg_internal
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
     * @dg_internal
     * @version 2.0.2
     */
    static std::string tri_get_tier_param(
        const std::string& name, const std::string& param, void* ud);
};

} // namespace entropic
