/**
 * @file worktree.h
 * @brief Git worktree management for delegation isolation.
 *
 * Ports Python's WorktreeManager and scoped_worktree(). Provides
 * branch-per-delegation isolation with develop as the integration
 * branch and main as the pristine reference.
 *
 * @version 1.8.6
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace entropic {

class AgentEngine; // forward declaration

/**
 * @brief Tracks a created worktree's path and branch.
 * @version 1.8.6
 */
struct WorktreeInfo {
    std::filesystem::path path;    ///< Worktree directory
    std::string branch;            ///< Branch name (e.g., "delegation/eng-abc12345")
    std::string delegation_id;    ///< Delegation UUID (short)
};

/**
 * @brief Result of a git command execution.
 * @version 1.8.6
 */
struct GitResult {
    bool success = false;  ///< Exit code == 0
    std::string output;    ///< Combined stdout
};

/**
 * @brief Create, merge, and discard git worktrees for delegation isolation.
 *
 * Ensures a `develop` branch exists before creating delegation worktrees.
 * The main working tree is checked out to `develop` so that lead operates
 * there. Delegation branches are created from and merged back to `develop`.
 * Main is only updated via `accept_to_main()`.
 *
 * @code
 * WorktreeManager mgr(repo_dir);
 * mgr.ensure_develop();
 * auto wt = mgr.create_worktree(id, "eng");
 * // ... child works in wt->path ...
 * mgr.merge_worktree(*wt);
 * mgr.accept_to_main();
 * @endcode
 *
 * @version 1.8.6
 */
class WorktreeManager {
public:
    /**
     * @brief Construct with repository root directory.
     * @param repo_dir Path to the git repository root.
     * @version 1.8.6
     */
    explicit WorktreeManager(const std::filesystem::path& repo_dir);

    /**
     * @brief Create develop branch from current HEAD and check it out.
     * @return true if the working tree is on develop.
     * @version 1.8.6
     */
    bool ensure_develop();

    /**
     * @brief Create a git worktree for a delegation, branching from develop.
     * @param delegation_id Short ID for the delegation.
     * @param tier Target tier name (used in branch naming).
     * @return WorktreeInfo on success, nullopt on failure.
     * @version 1.8.6
     */
    std::optional<WorktreeInfo> create_worktree(
        const std::string& delegation_id,
        const std::string& tier);

    /**
     * @brief Merge worktree branch back to develop.
     * @param info Worktree to merge.
     * @return true if merge succeeded.
     * @version 1.8.6
     */
    bool merge_worktree(const WorktreeInfo& info);

    /**
     * @brief Remove worktree and delete branch without merging.
     * @param info Worktree to discard.
     * @version 1.8.6
     */
    void discard_worktree(const WorktreeInfo& info);

    /**
     * @brief Merge develop into main. Called on explicit acceptance.
     * @return true if merge succeeded.
     * @version 1.8.6
     */
    bool accept_to_main();

    /**
     * @brief Discard develop branch, returning to main.
     * @return true if discard succeeded.
     * @version 1.8.6
     */
    bool discard_develop();

    /**
     * @brief Get the repository root directory.
     * @return Repo root path.
     * @version 1.8.6
     */
    const std::filesystem::path& repo_dir() const;

private:
    /**
     * @brief Auto-commit uncommitted changes before merge.
     * @param info Worktree to check for dirty state.
     * @version 1.8.6
     */
    void auto_commit_if_dirty(const WorktreeInfo& info);

    /**
     * @brief Remove worktree directory and delete branch.
     * @param info Worktree to clean up.
     * @version 1.8.6
     */
    void cleanup(const WorktreeInfo& info);

    /**
     * @brief Find the main branch name (main or master).
     * @return Branch name, or empty string if not found.
     * @version 1.8.6
     */
    std::string find_main_branch() const;

    std::filesystem::path repo_dir_;       ///< Repository root
    std::filesystem::path worktree_base_;  ///< .worktrees/ directory
    bool develop_ready_ = false;           ///< develop branch checked out
};

/**
 * @brief RAII worktree directory swapper.
 *
 * Swaps filesystem root_dir, bash working_dir, and git repo_dir
 * to the worktree path. Restores all on destruction (success or failure).
 * Uses entropic_mcp_server_set_working_dir() per-server via the engine's
 * server access callback.
 *
 * @version 1.8.6
 */
class ScopedWorktree {
public:
    /**
     * @brief Callback type for directory swapping.
     *
     * The facade implements this to iterate servers and call
     * entropic_mcp_server_set_working_dir() on each.
     *
     * @param path New working directory.
     * @param user_data Opaque pointer (facade context).
     * @version 1.8.6
     */
    using SwapDirFn = void (*)(
        const std::filesystem::path& path, void* user_data);

    /**
     * @brief Construct and swap directories.
     * @param swap_fn Callback to swap all server directories.
     * @param user_data Opaque pointer for swap_fn.
     * @param worktree_path Target worktree directory.
     * @param original_path Original working directory to restore.
     * @version 1.8.6
     */
    ScopedWorktree(SwapDirFn swap_fn,
                   void* user_data,
                   const std::filesystem::path& worktree_path,
                   const std::filesystem::path& original_path);

    /**
     * @brief Restore original directories.
     * @version 1.8.6
     */
    ~ScopedWorktree();

    ScopedWorktree(const ScopedWorktree&) = delete;
    ScopedWorktree& operator=(const ScopedWorktree&) = delete;

private:
    SwapDirFn swap_fn_;                    ///< Directory swap callback
    void* user_data_;                      ///< Opaque pointer
    std::filesystem::path original_path_;  ///< Path to restore
};

/**
 * @brief Run a git command in a directory and capture output.
 * @param repo_dir Working directory for the command.
 * @param args Git subcommand and arguments.
 * @return GitResult with success flag and output.
 * @version 1.8.6
 */
GitResult run_git(const std::filesystem::path& repo_dir,
                  const std::string& args);

} // namespace entropic
