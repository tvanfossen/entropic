/**
 * @file delegation.h
 * @brief DelegationManager — child loop creation and execution.
 *
 * Ports Python's DelegationManager. Orchestrates child inference loop
 * creation, worktree lifecycle, pipeline execution, and todo list
 * save/restore across delegation boundaries.
 *
 * @version 1.8.6
 */

#pragma once

#include <entropic/core/engine_types.h>
#include <entropic/core/worktree.h>
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
    bool success = false;                 ///< Whether child reached COMPLETE
    std::string target_tier;              ///< Tier that executed
    std::string task;                     ///< Original task text
    int turns_used = 0;                   ///< Iterations consumed
    std::vector<Message> child_messages;  ///< Full child message history
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
     * @brief Set directory swap callback for ScopedWorktree.
     * @param swap_fn Directory swap callback.
     * @param user_data Opaque pointer for swap_fn.
     * @version 1.8.6
     */
    void set_dir_swap(ScopedWorktree::SwapDirFn swap_fn, void* user_data);

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
     * @brief Finalize worktree based on delegation result.
     * @param wt_info Worktree to finalize (nullopt if no worktree).
     * @param result Delegation result determining merge/discard.
     * @version 1.8.6
     */
    void finalize_worktree(
        const std::optional<WorktreeInfo>& wt_info,
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
    ScopedWorktree::SwapDirFn swap_dir_fn_ = nullptr;  ///< Dir swap callback
    void* swap_dir_data_ = nullptr;                    ///< Dir swap user data
    std::optional<WorktreeManager> worktree_mgr_;      ///< Git worktree manager
    std::filesystem::path repo_dir_;                   ///< Repository root
    const struct StorageInterface* storage_ = nullptr; ///< Nullable storage (v1.8.8)
};

} // namespace entropic
