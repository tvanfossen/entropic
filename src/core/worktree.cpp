/**
 * @file worktree.cpp
 * @brief WorktreeManager and ScopedWorktree implementation.
 *
 * Git operations shell out via popen() (matching Python behavior).
 * Each run_git() call captures stdout and returns {success, output}.
 *
 * @version 1.8.6
 */

#include <entropic/core/worktree.h>
#include <entropic/types/logging.h>

#include <array>
#include <cstdio>

static auto logger = entropic::log::get("core.worktree");

namespace entropic {

// ── run_git ──────────────────────────────────────────────

/**
 * @brief Run a git command in a directory and capture output.
 * @param repo_dir Working directory for the command.
 * @param args Git subcommand and arguments.
 * @return GitResult with success flag and output.
 * @utility
 * @version 1.8.6
 */
GitResult run_git(const std::filesystem::path& repo_dir,
                  const std::string& args) {
    std::string cmd = "cd '" + repo_dir.string() +
                      "' && git " + args + " 2>&1";

    GitResult result;
    std::array<char, 256> buf{};

    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        logger->error("popen failed for: git {}", args);
        return result;
    }

    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        result.output += buf.data();
    }

    int status = pclose(pipe);
    result.success = (status == 0);
    return result;
}

// ── WorktreeManager ──────────────────────────────────────

/**
 * @brief Construct with repository root directory.
 * @param repo_dir Path to the git repository root.
 * @internal
 * @version 1.8.6
 */
WorktreeManager::WorktreeManager(const std::filesystem::path& repo_dir)
    : repo_dir_(repo_dir),
      worktree_base_(repo_dir / ".worktrees") {
    logger->info("WorktreeManager: repo={}", repo_dir_.string());
}

/**
 * @brief Get the repository root directory.
 * @return Repo root path.
 * @internal
 * @version 1.8.6
 */
const std::filesystem::path& WorktreeManager::repo_dir() const {
    return repo_dir_;
}

/**
 * @brief Find the main branch name (main or master).
 * @return Branch name, or empty string if not found.
 * @internal
 * @version 1.8.6
 */
std::string WorktreeManager::find_main_branch() const {
    auto r = run_git(repo_dir_, "branch --list main");
    if (r.success && r.output.find("main") != std::string::npos) {
        return "main";
    }
    r = run_git(repo_dir_, "branch --list master");
    if (r.success && r.output.find("master") != std::string::npos) {
        return "master";
    }
    return "";
}

/**
 * @brief Create develop branch from current HEAD and check it out.
 * @return true if the working tree is on develop.
 * @internal
 * @version 1.8.6
 */
bool WorktreeManager::ensure_develop() {
    if (develop_ready_) {
        return true;
    }

    auto check = run_git(repo_dir_, "branch --list develop");
    bool exists = check.output.find("develop") != std::string::npos;
    std::string cmd = exists ? "checkout develop" : "checkout -b develop";

    auto result = run_git(repo_dir_, cmd);
    if (!result.success) {
        logger->error("Failed to setup develop branch");
        return false;
    }

    if (!exists) {
        logger->info("Created develop branch from HEAD");
    }
    develop_ready_ = true;
    return true;
}

/**
 * @brief Create a git worktree for a delegation.
 * @param delegation_id Short ID for the delegation.
 * @param tier Target tier name.
 * @return WorktreeInfo on success, nullopt on failure.
 * @internal
 * @version 1.8.6
 */
std::optional<WorktreeInfo> WorktreeManager::create_worktree(
    const std::string& delegation_id,
    const std::string& tier) {
    if (!develop_ready_) {
        logger->error("Cannot create worktree: develop not ready");
        return std::nullopt;
    }

    std::string short_id = delegation_id.substr(
        0, std::min(delegation_id.size(), size_t{8}));
    std::string branch = "delegation/" + tier + "-" + short_id;
    auto path = worktree_base_ / ("delegation-" + short_id);

    std::string cmd = "worktree add -b " + branch +
                      " '" + path.string() + "' develop";
    auto r = run_git(repo_dir_, cmd);
    if (!r.success) {
        logger->error("Failed to create worktree: {}", r.output);
        return std::nullopt;
    }

    logger->info("Created worktree: {} -> {}", branch, path.string());
    return WorktreeInfo{path, branch, short_id};
}

/**
 * @brief Auto-commit uncommitted changes before merge.
 * @param info Worktree to check.
 * @internal
 * @version 1.8.6
 */
void WorktreeManager::auto_commit_if_dirty(const WorktreeInfo& info) {
    auto status = run_git(info.path, "status --porcelain");
    if (!status.success || status.output.empty()) {
        return;
    }

    run_git(info.path, "add -A");
    run_git(info.path,
            "commit -m 'Auto-commit before merge (" +
            info.delegation_id + ")'");
    logger->info("Auto-committed dirty worktree: {}", info.branch);
}

/**
 * @brief Merge worktree branch back to develop.
 * @param info Worktree to merge.
 * @return true if merge succeeded.
 * @internal
 * @version 1.8.6
 */
bool WorktreeManager::merge_worktree(const WorktreeInfo& info) {
    auto_commit_if_dirty(info);

    auto merge = run_git(repo_dir_,
                         "merge --no-ff " + info.branch +
                         " -m 'Merge delegation " +
                         info.delegation_id + "'");
    bool ok = merge.success;

    if (!ok) {
        logger->error("Merge failed for {}: {}", info.branch, merge.output);
    } else {
        logger->info("Merged {} into develop", info.branch);
    }

    cleanup(info);
    return ok;
}

/**
 * @brief Remove worktree directory and delete branch.
 * @param info Worktree to clean up.
 * @internal
 * @version 1.8.6
 */
void WorktreeManager::cleanup(const WorktreeInfo& info) {
    run_git(repo_dir_, "worktree remove --force '" +
                       info.path.string() + "'");
    run_git(repo_dir_, "branch -D " + info.branch);
    logger->info("Cleaned up worktree: {}", info.branch);
}

/**
 * @brief Remove worktree and delete branch without merging.
 * @param info Worktree to discard.
 * @internal
 * @version 1.8.6
 */
void WorktreeManager::discard_worktree(const WorktreeInfo& info) {
    logger->info("Discarding worktree: {}", info.branch);
    cleanup(info);
}

/**
 * @brief Merge develop into main.
 * @return true if merge succeeded.
 * @internal
 * @version 1.8.6
 */
bool WorktreeManager::accept_to_main() {
    std::string main_branch = find_main_branch();
    if (main_branch.empty()) {
        logger->error("No main/master branch found");
        return false;
    }

    auto co = run_git(repo_dir_, "checkout " + main_branch);
    if (!co.success) {
        return false;
    }

    auto merge = run_git(repo_dir_, "merge --no-ff develop "
                                    "-m 'Accept develop into " +
                                    main_branch + "'");
    run_git(repo_dir_, "checkout develop");

    if (!merge.success) {
        logger->error("accept_to_main failed: {}", merge.output);
    }
    return merge.success;
}

/**
 * @brief Discard develop branch, returning to main.
 * @return true if discard succeeded.
 * @internal
 * @version 1.8.6
 */
bool WorktreeManager::discard_develop() {
    std::string main_branch = find_main_branch();
    if (main_branch.empty()) {
        return false;
    }

    auto co = run_git(repo_dir_, "checkout " + main_branch);
    if (!co.success) {
        return false;
    }

    auto del = run_git(repo_dir_, "branch -D develop");
    develop_ready_ = false;
    return del.success;
}

// ── ScopedWorktree ───────────────────────────────────────

/**
 * @brief Construct and swap directories.
 * @param swap_fn Callback to swap all server directories.
 * @param user_data Opaque pointer for swap_fn.
 * @param worktree_path Target worktree directory.
 * @param original_path Original working directory to restore.
 * @internal
 * @version 1.8.6
 */
ScopedWorktree::ScopedWorktree(
    SwapDirFn swap_fn,
    void* user_data,
    const std::filesystem::path& worktree_path,
    const std::filesystem::path& original_path)
    : swap_fn_(swap_fn),
      user_data_(user_data),
      original_path_(original_path) {
    if (swap_fn_ != nullptr) {
        swap_fn_(worktree_path, user_data_);
        logger->info("Swapped dirs to: {}", worktree_path.string());
    }
}

/**
 * @brief Restore original directories.
 * @internal
 * @version 1.8.6
 */
ScopedWorktree::~ScopedWorktree() {
    if (swap_fn_ != nullptr) {
        swap_fn_(original_path_, user_data_);
        logger->info("Restored dirs to: {}", original_path_.string());
    }
}

} // namespace entropic
