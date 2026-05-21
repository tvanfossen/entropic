// SPDX-License-Identifier: Apache-2.0
/**
 * @file delegation.h
 * @brief DelegationManager — child loop creation and execution.
 *
 * Ports Python's DelegationManager. Orchestrates child inference loop
 * creation, sandbox lifecycle, pipeline execution, and todo list
 * save/restore across delegation boundaries.
 *
 * @version 2.1.5
 */

#pragma once

#include <entropic/entropic.h>  // ent_decision_t + ent_delegation_* (gh#29, v2.1.5)
#include <entropic/core/engine_types.h>
#include <entropic/core/sandbox.h>
#include <entropic/types/message.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace entropic {

class AgentEngine; // forward declaration

/**
 * @brief Result returned from a child delegation loop.
 * @version 1.8.6
 */
struct DelegationResult {
    std::string summary;                  ///< Final summary from child
    bool success = false;                 ///< Whether child reached COMPLETE via real entropic.complete
    std::string target_tier;              ///< Tier that executed
    std::string task;                     ///< Original task text
    int turns_used = 0;                   ///< Iterations consumed
    std::vector<Message> child_messages;  ///< Full child message history
    /// @brief Non-empty when the child terminated synthetically rather
    /// than via a real entropic.complete. Current value: "budget_exhausted"
    /// — set by AgentEngine::loop when max_iterations is hit and a
    /// forced synthetic complete is injected. Empty string = natural
    /// completion. (E7, 2.0.6-rc18)
    std::string terminal_reason;
    /// @brief Issue #10 (v2.1.4): coverage_gap signal from the child's
    /// entropic.complete call. When true, the parent engine SUPPRESSES
    /// auto-relay and instead injects a `[COVERAGE GAP]` user message
    /// into the lead context so lead can chain to a follow-up specialist.
    bool coverage_gap = false;
    /// @brief Issue #10 (v2.1.4): concrete description of what the
    /// child's answer DOES NOT cover. Required when coverage_gap=true
    /// (validated at the tool boundary in CompleteTool::execute).
    std::string gap_description;
    /// @brief Issue #10 (v2.1.4): file paths the lead should inspect
    /// to fill the coverage gap. Only populated when coverage_gap=true.
    std::vector<std::string> suggested_files;
};

/**
 * @brief Callback type for running a child engine loop.
 *
 * The engine provides this so DelegationManager can spawn child loops
 * without a circular dependency on AgentEngine's full interface.
 *
 * @param ctx Child loop context.
 * @param user_data Opaque pointer (AgentEngine instance).
 * @version 1.8.6
 */
using RunChildLoopFn = void (*)(LoopContext& ctx, void* user_data);

/**
 * @brief Callback type for saving/restoring todo list state.
 *
 * The facade implements these to access EntropicServer's TodoTool.
 *
 * @version 1.8.6
 */
struct TodoCallbacks {
    /// @brief Save current todo list state. Returns opaque state string.
    std::string (*save)(void* user_data) = nullptr;
    /// @brief Install a fresh empty todo list for child.
    void (*install_fresh)(void* user_data) = nullptr;
    /// @brief Restore a previously saved todo list state.
    void (*restore)(const std::string& saved, void* user_data) = nullptr;
    void* user_data = nullptr; ///< Opaque pointer (facade context)
};

/**
 * @brief Orchestrates child loop creation and execution.
 *
 * Builds a fresh LoopContext for the target tier, runs the engine's
 * loop to completion, and extracts the final assistant message as
 * the delegation result. Manages worktree lifecycle (create, merge,
 * discard) and todo list save/restore across child contexts.
 *
 * @code
 * DelegationManager mgr(run_fn, ud, tier_res, repo_dir);
 * auto result = mgr.execute_delegation(parent_ctx, "eng", "Implement X");
 * // result.summary contains the child's final output
 * @endcode
 *
 * @version 1.8.6
 */
class DelegationManager {
public:
    /**
     * @brief Construct with engine loop callback and tier resolution.
     *
     * gh#33 (v2.1.6): pre-2.1.6 this constructor owned a fresh
     * `SandboxManager` per delegation. The manager is now engine-scoped
     * and supplied as a non-owning pointer by the caller (AgentEngine);
     * passing nullptr disables sandboxing for the delegation.
     *
     * @param run_child Callback to run child engine loop.
     * @param run_child_data Opaque pointer for run_child.
     * @param tier_resolution Tier resolution interface.
     * @param repo_dir Optional repo root (informational; used for
     *        ScopedSandbox dir-swap and storage record metadata).
     * @param sandbox_mgr Non-owning, engine-scoped sandbox manager. May
     *        be nullptr (delegation runs without an isolated sandbox).
     * @version 2.1.6
     */
    DelegationManager(RunChildLoopFn run_child,
                      void* run_child_data,
                      const TierResolutionInterface& tier_resolution,
                      const std::filesystem::path& repo_dir = {},
                      SandboxManager* sandbox_mgr = nullptr);

    /**
     * @brief Set todo list save/restore callbacks.
     * @param callbacks Todo callbacks.
     * @version 1.8.6
     */
    void set_todo_callbacks(const TodoCallbacks& callbacks);

    /**
     * @brief Set directory swap callback for ScopedSandbox.
     * @param swap_fn Directory swap callback.
     * @param user_data Opaque pointer for swap_fn.
     * @version 2.1.5
     */
    void set_dir_swap(ScopedSandbox::SwapDirFn swap_fn, void* user_data);

    /**
     * @brief Set storage interface for delegation record persistence.
     * @param storage Storage callbacks (nullable).
     * @version 1.8.8
     */
    void set_storage(const struct StorageInterface* storage);

    /**
     * @brief Set delegation start/complete callbacks (gh#29, v2.1.5).
     *
     * Forwarded by the engine from `entropic_set_delegation_callbacks`.
     * `on_start` is invoked before each child loop runs and may veto
     * the delegation (REJECT). `on_complete` receives the sandbox patch
     * artifact; ACCEPT means the consumer applied it (sandbox is
     * discarded), REJECT or NULL means the engine writes the patch to
     * `<session>/pending/<id>.patch` as a default-deny fallback.
     *
     * @param on_start    Pre-delegation gate (nullable).
     * @param on_complete Post-delegation result (nullable).
     * @param user_data   Forwarded to both callbacks.
     * @version 2.1.5
     */
    void set_delegation_callbacks(
        ent_decision_t (*on_start)(const ent_delegation_request_t*, void*),
        ent_decision_t (*on_complete)(const ent_delegation_result_t*, void*),
        void* user_data);

    /**
     * @brief Run a child inference loop for the target tier.
     * @param parent_ctx Parent loop context.
     * @param target_tier Tier name to delegate to.
     * @param task Task description for the child.
     * @param max_turns Optional iteration limit for child loop.
     * @return DelegationResult with summary, success, and child messages.
     * @version 1.8.6
     */
    DelegationResult execute_delegation(
        LoopContext& parent_ctx,
        const std::string& target_tier,
        const std::string& task,
        std::optional<int> max_turns = std::nullopt);

    /**
     * @brief Resume a prior delegation with pre-loaded conversation history.
     *
     * gh#32 (v2.1.6): Seeds the child context with `seed_history`
     * (loaded from storage by the engine), then appends the supplied
     * `task` as a fresh user message. Subsequent run_child semantics
     * are identical to `execute_delegation`. The first system message
     * from the loaded history is preserved; if absent, the tier's
     * default system prompt is prepended so the child still has its
     * identity context.
     *
     * @param parent_ctx    Parent loop context.
     * @param target_tier   Tier resolved by the engine from storage.
     * @param task          New sub-task to append to history.
     * @param seed_history  Pre-loaded conversation messages.
     * @param max_turns     Optional iteration limit.
     * @return DelegationResult.
     * @version 2.1.6
     */
    DelegationResult execute_resume_delegation(
        LoopContext& parent_ctx,
        const std::string& target_tier,
        const std::string& task,
        std::vector<Message> seed_history,
        std::optional<int> max_turns = std::nullopt);

    /**
     * @brief Run a multi-stage delegation pipeline sequentially.
     * @param parent_ctx Parent loop context.
     * @param stages Ordered list of tier names.
     * @param task Task description (shared across stages).
     * @return DelegationResult from the final stage.
     * @version 1.8.6
     */
    DelegationResult execute_pipeline(
        LoopContext& parent_ctx,
        const std::vector<std::string>& stages,
        const std::string& task);

private:
    /**
     * @brief Build a fresh LoopContext for the child delegation.
     * @param parent_ctx Parent context.
     * @param info Resolved tier info.
     * @param task Task description.
     * @return Fresh child context.
     * @version 1.8.6
     */
    LoopContext build_child_context(
        const LoopContext& parent_ctx,
        const ChildContextInfo& info,
        const std::string& task);

    /**
     * @brief Build a resumed child context (gh#32, v2.1.6).
     *
     * Seeds with prior conversation, appends the tier's identity if no
     * system prompt exists, then appends the new task.
     *
     * @param parent_ctx     Parent loop context.
     * @param info           Resolved tier info.
     * @param target_tier    Tier name.
     * @param task           New sub-task.
     * @param seed_history   Loaded history (consumed).
     * @return Resumed child context.
     * @internal
     * @version 2.1.6
     */
    LoopContext build_resumed_child_context(
        const LoopContext& parent_ctx,
        const ChildContextInfo& info,
        const std::string& target_tier,
        const std::string& task,
        std::vector<Message> seed_history);

    /**
     * @brief Extract the delegation summary from child context.
     * @param child_ctx Completed child context.
     * @return Summary text.
     * @version 1.8.6
     */
    std::string extract_summary(const LoopContext& child_ctx) const;

    /**
     * @brief Run the three pre-flight checks for a single delegation.
     *
     * Returns a populated `DelegationResult` (caller should early-exit)
     * or `nullopt` (proceed). On success with a configured sandbox,
     * `sb_info_out` is populated with the freshly created sandbox info.
     * Extracted to keep `execute_delegation` under knots SLOC/return
     * limits (gh#33, v2.1.6).
     *
     * @param info          Resolved tier info.
     * @param target_tier   Tier name.
     * @param task          Task description.
     * @param del_id        Delegation id (e.g. "d1").
     * @param depth         Parent depth + 1.
     * @param sb_info_out   [out] Sandbox info populated on success.
     * @return DelegationResult to early-return, or nullopt to proceed.
     * @version 2.1.6
     */
    std::optional<DelegationResult> check_delegation_preconditions(
        const ChildContextInfo& info,
        const std::string& target_tier,
        const std::string& task,
        const std::string& del_id,
        int depth,
        std::optional<SandboxInfo>& sb_info_out);

    /**
     * @brief Run child loop with todo save/restore.
     * @param child_ctx Child context to execute.
     * @param target_tier Tier name.
     * @param task Task description.
     * @param max_turns Optional turn limit.
     * @return DelegationResult.
     * @version 1.8.6
     */
    DelegationResult run_child(
        LoopContext& child_ctx,
        const std::string& target_tier,
        const std::string& task,
        std::optional<int> max_turns);

    /**
     * @brief Build DelegationResult from a terminated child context.
     * @param target_tier Tier that executed.
     * @param task Task text.
     * @param child_ctx Terminated child context (messages moved out).
     * @return DelegationResult with terminal_reason/success populated.
     * @internal
     * @version 2.0.6-rc18
     */
    DelegationResult build_child_result(
        const std::string& target_tier,
        const std::string& task,
        LoopContext& child_ctx);

    /**
     * @brief Emit "Child loop done:" log branched on terminal_reason.
     * @param result Delegation result with terminal_reason populated.
     * @internal
     * @version 2.0.6-rc18
     */
    void log_child_result(const DelegationResult& result);

    /**
     * @brief Finalize sandbox based on delegation result.
     *
     * On success, generates a unified-diff patch via the sandbox
     * manager. The patch is currently discarded after logging — the
     * 2.1.5 fix removes the previous auto-merge-to-parent behavior
     * (gh#29). Consumer-facing delivery of the patch will land via
     * the delegation-complete C ABI callback (also 2.1.5).
     *
     * @param sb_info Sandbox to finalize (nullopt if no sandbox).
     * @param result Delegation result (success/failure governs cleanup).
     * @version 2.1.5
     */
    void finalize_sandbox_for(
        const std::optional<SandboxInfo>& sb_info,
        const DelegationResult& result);

    /**
     * @brief Create delegation storage record.
     * @param child_ctx Child context.
     * @param target_tier Target tier.
     * @param task Task description.
     * @param max_turns Turn limit.
     * @return Delegation ID (empty if no storage).
     * @version 1.8.8
     */
    std::string create_storage_record(
        LoopContext& child_ctx, const std::string& target_tier,
        const std::string& task, std::optional<int> max_turns);

    /**
     * @brief Complete delegation storage record.
     * @param delegation_id Delegation ID.
     * @param result Delegation result.
     * @version 1.8.8
     */
    void complete_storage_record(
        const std::string& delegation_id,
        const DelegationResult& result);

    RunChildLoopFn run_child_fn_;                      ///< Engine loop callback
    void* run_child_data_;                             ///< Engine instance
    TierResolutionInterface tier_res_;                 ///< Tier lookup callbacks
    TodoCallbacks todo_callbacks_;                     ///< Todo save/restore
    ScopedSandbox::SwapDirFn swap_dir_fn_ = nullptr;   ///< Dir swap callback
    void* swap_dir_data_ = nullptr;                    ///< Dir swap user data
    SandboxManager* sandbox_mgr_ = nullptr;            ///< gh#33 (v2.1.6): non-owning, engine-scoped
    std::filesystem::path repo_dir_;                   ///< Project root
    const struct StorageInterface* storage_ = nullptr; ///< Nullable storage (v1.8.8)

    // ── Delegation callbacks (gh#29, v2.1.5) ───────────────
    ent_decision_t (*delegation_start_cb_)(
        const ent_delegation_request_t*, void*) = nullptr;     ///< Pre-delegation gate
    ent_decision_t (*delegation_complete_cb_)(
        const ent_delegation_result_t*, void*) = nullptr;      ///< Post-delegation result
    void* delegation_cb_data_ = nullptr;                       ///< Forwarded to both

    /**
     * @brief Invoke `delegation_start_cb_` if set; return its decision.
     *
     * Builds an `ent_delegation_request_t` from the supplied parameters
     * and dispatches to the registered start callback. Null callback
     * defaults to `ENT_DECISION_ACCEPT`.
     *
     * @param delegation_id Short id (e.g. "d1", "pipeline").
     * @param target_tier   Tier name.
     * @param task          Task description.
     * @param depth         Delegation depth.
     * @param is_pipeline   True if this is a pipeline stage.
     * @return Decision returned by the callback (or ACCEPT if null).
     * @internal
     * @version 2.1.5
     */
    ent_decision_t fire_start_cb(
        const std::string& delegation_id,
        const std::string& target_tier,
        const std::string& task,
        int depth,
        bool is_pipeline);

    /**
     * @brief Dispatch a finalized sandbox result to consumer or pending.
     *
     * Materializes `ent_delegation_result_t` from `SandboxResult` and
     * fires `delegation_complete_cb_`. On REJECT (or null callback)
     * writes the patch to `<session>/pending/<id>.patch`. The sandbox
     * directory is removed by the caller after this returns.
     *
     * @param sb_info        Sandbox identity (for delegation_id).
     * @param sandbox_result Patch artifact from finalize_sandbox().
     * @param result         Original DelegationResult (for success/summary).
     * @internal
     * @version 2.1.5
     */
    void deliver_sandbox_result(
        const SandboxInfo& sb_info,
        const SandboxResult& sandbox_result,
        const DelegationResult& result);

    /**
     * @brief Invoke delegation_complete_cb_ under an exception shield.
     *
     * Extracted from deliver_sandbox_result. A throwing consumer is
     * treated as REJECT so the patch is preserved (gh#29 hardening).
     *
     * @param res Filled result struct.
     * @param delegation_id For the warn log on throw.
     * @return The consumer's decision (REJECT on throw).
     * @internal
     * @version 2.3.7
     */
    ent_decision_t invoke_complete_cb(const ent_delegation_result_t& res,
                                      const std::string& delegation_id);

    /**
     * @brief Persist a patch to pending/ + log a WARN message.
     *
     * Extracted from `deliver_sandbox_result` to keep that function
     * under the SLOC complexity gate. Used in two cases: no complete
     * callback registered, or callback returned REJECT.
     *
     * @param sb_info Sandbox identity (for delegation_id + log fmt).
     * @param sandbox_result Patch artifact.
     * @param reason Short reason string for the log line.
     * @internal
     * @version 2.1.5
     */
    void persist_pending_patch(
        const SandboxInfo& sb_info,
        const SandboxResult& sandbox_result,
        const char* reason);

    /**
     * @brief Run one pipeline stage; returns false to stop pipeline.
     *
     * Extracted from `execute_pipeline` to keep that function under
     * the SLOC complexity gate. Resolves the tier, builds the child
     * context, runs the child (under the shared sandbox if available),
     * updates `last_result`, and logs the stage outcome.
     *
     * @param parent_ctx Parent loop context.
     * @param stages     Full stage list (for indexing/labeling).
     * @param stage_idx  Index of the stage to execute.
     * @param task       Original task text.
     * @param shared_sb  Shared pipeline sandbox (optional).
     * @param last_result In/out: prior-stage result on entry, this
     *                    stage's result on return.
     * @return false if the pipeline should stop (failure or unknown
     *         tier); true to continue to the next stage.
     * @internal
     * @version 2.1.5
     */
    bool run_pipeline_stage(
        LoopContext& parent_ctx,
        const std::vector<std::string>& stages,
        size_t stage_idx,
        const std::string& task,
        const std::optional<SandboxInfo>& shared_sb,
        DelegationResult& last_result);
};

} // namespace entropic
