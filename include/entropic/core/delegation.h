// SPDX-License-Identifier: LGPL-3.0-or-later
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
     * @param run_child Callback to run child engine loop.
     * @param run_child_data Opaque pointer for run_child.
     * @param tier_resolution Tier resolution interface.
     * @param repo_dir Optional repo root for worktree isolation.
     * @version 1.8.6
     */
    DelegationManager(RunChildLoopFn run_child,
                      void* run_child_data,
                      const TierResolutionInterface& tier_resolution,
                      const std::filesystem::path& repo_dir = {});

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
     * @brief Extract the delegation summary from child context.
     * @param child_ctx Completed child context.
     * @return Summary text.
     * @version 1.8.6
     */
    std::string extract_summary(const LoopContext& child_ctx) const;

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
    std::optional<SandboxManager> sandbox_mgr_;        ///< Filesystem sandbox (2.1.5, gh#29)
    std::filesystem::path repo_dir_;                   ///< Project root
    const struct StorageInterface* storage_ = nullptr; ///< Nullable storage (v1.8.8)
};

} // namespace entropic
