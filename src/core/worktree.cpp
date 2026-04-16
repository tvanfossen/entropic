// SPDX-License-Identifier: LGPL-3.0-or-later
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
#include <random>
#include <set>
#include <sstream>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

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
 * @brief Generate a session id of the form "<pid>-<hex8>".
 *
 * pid is sufficient on its own for liveness detection, but adding
 * random hex avoids collisions when a stale dir lingers after a
 * fast pid-recycle.
 *
 * @return Session identifier string.
 * @utility
 * @version 2.0.6
 */
static std::string make_session_id() {
    std::random_device rd;
    std::uniform_int_distribution<uint32_t> dist;
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", dist(rd));
    return std::to_string(static_cast<long>(getpid())) + "-" + hex;
}

/**
 * @brief Construct with repository root directory.
 *
 * Creates a session-scoped worktree base at
 * `.worktrees/<session_id>/` and prunes stale dirs from prior
 * sessions.
 *
 * @param repo_dir Path to the git repository root.
 * @internal
 * @version 2.0.6
 */
WorktreeManager::WorktreeManager(const std::filesystem::path& repo_dir)
    : repo_dir_(repo_dir),
      session_id_(make_session_id()),
      worktree_base_(repo_dir / ".worktrees" / session_id_) {
    logger->info("WorktreeManager: repo={} session={}",
                 repo_dir_.string(), session_id_);
    prune_stale_worktrees();
    std::error_code ec;
    std::filesystem::create_directories(worktree_base_, ec);
    if (ec) {
        logger->error("Failed to create worktree base {}: {}",
                      worktree_base_.string(), ec.message());
    }
}

/**
 * @brief Destructor — remove this session's worktree base directory.
 *
 * Complements per-worktree cleanup; any delegation directory that
 * leaked past `cleanup()` is removed when the session ends.
 *
 * @internal
 * @version 2.0.6
 */
WorktreeManager::~WorktreeManager() {
    std::error_code ec;
    std::filesystem::remove_all(worktree_base_, ec);
    if (ec) {
        logger->warn("Failed to remove session worktree base {}: {}",
                     worktree_base_.string(), ec.message());
    } else {
        logger->info("Removed session worktree base: {}",
                     worktree_base_.string());
    }
}

/**
 * @brief Check whether the given process id is still alive.
 *
 * Uses `kill(pid, 0)`. Returns false only when errno == ESRCH
 * (owning process is gone). EPERM (different UID) counts as alive —
 * we do not own that session's dir and must leave it.
 *
 * @param pid Process id to probe.
 * @return true if alive or not ours; false if the owner is gone.
 * @utility
 * @version 2.0.6
 */
static bool pid_is_alive(long pid) {
    if (kill(static_cast<pid_t>(pid), 0) == 0) { return true; }
    return errno != ESRCH;
}

/**
 * @brief Remove a directory, logging the outcome.
 * @param dir Directory to remove.
 * @param reason Human-readable reason (for log line).
 * @utility
 * @version 2.0.6
 */
static void remove_orphan_dir(
    const std::filesystem::path& dir, const std::string& reason) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec) {
        logger->warn("Failed to remove orphan {} ({}): {}",
                     dir.string(), reason, ec.message());
    } else {
        logger->info("Pruned orphan {} ({})", dir.string(), reason);
    }
}

/**
 * @brief Parse "<pid>-<hex>" session-id directory name into a pid.
 * @param name Directory basename.
 * @param[out] pid Extracted pid on success.
 * @return true if `name` matches the session-id pattern.
 * @utility
 * @version 2.0.6
 */
static bool parse_session_pid(const std::string& name, long& pid) {
    auto dash = name.find('-');
    if (dash == std::string::npos || dash == 0) { return false; }
    try {
        size_t consumed = 0;
        pid = std::stol(name.substr(0, dash), &consumed);
        return consumed == dash && name.size() - dash - 1 == 8;
    } catch (const std::exception&) {
        return false;
    }
}

/**
 * @brief Collect delegation branch names currently in git.
 * @return Set of branch names, trimmed and filtered to delegation/*.
 * @utility
 * @version 2.0.6
 */
std::set<std::string> WorktreeManager::collect_delegation_branches() const {
    std::set<std::string> result;
    auto branches = run_git(repo_dir_, "branch --list 'delegation/*'");
    if (!branches.success || branches.output.empty()) { return result; }

    std::istringstream iss(branches.output);
    std::string line;
    while (std::getline(iss, line)) {
        auto start = line.find_first_not_of(" *+");
        if (start == std::string::npos) { continue; }
        std::string branch = line.substr(start);
        auto end = branch.find_first_of(" \t");
        if (end != std::string::npos) { branch = branch.substr(0, end); }
        if (branch.rfind("delegation/", 0) == 0) {
            result.insert(std::move(branch));
        }
    }
    return result;
}

/**
 * @brief Prune one entry in .worktrees/ if it is a recognizable orphan.
 * @param entry Directory entry under .worktrees/.
 * @utility
 * @version 2.0.6
 */
void WorktreeManager::prune_worktrees_entry(
    const std::filesystem::directory_entry& entry) {
    if (!entry.is_directory()) { return; }
    std::string name = entry.path().filename().string();
    if (name == session_id_) { return; } // our own

    long pid = 0;
    if (parse_session_pid(name, pid)) {
        if (!pid_is_alive(pid)) {
            remove_orphan_dir(entry.path(),
                              "dead session pid=" + std::to_string(pid));
        }
        return;
    }

    // Pre-v2.0.6 layout: `.worktrees/delegation-<id>/` sat directly
    // under .worktrees. Anything matching that prefix here is an
    // orphan from before the session-scoped layout.
    if (name.rfind("delegation-", 0) == 0) {
        remove_orphan_dir(entry.path(), "pre-v2.0.6 layout");
    }
}

/**
 * @brief Remove stale delegation worktrees and branches from prior sessions.
 *
 * Called at startup to clean up any worktrees/branches left behind by
 * crashed or interrupted sessions. Without this, `worktree add -b` fails
 * with "branch already exists" and blocks all delegation.
 *
 * Walk order:
 * 1. `git worktree prune` — sync git admin state with filesystem.
 * 2. Iterate `.worktrees/*`: remove old-layout and dead-session dirs
 *    that git doesn't recognize as worktrees.
 * 3. Delete `delegation/*` branches, after pruning their backing dirs.
 *
 * @internal
 * @version 2.0.6
 */
void WorktreeManager::prune_stale_worktrees() {
    run_git(repo_dir_, "worktree prune");

    auto base = repo_dir_ / ".worktrees";
    std::error_code ec;
    if (std::filesystem::exists(base, ec)) {
        for (const auto& entry :
             std::filesystem::directory_iterator(base, ec)) {
            prune_worktrees_entry(entry);
        }
    }

    auto branches = collect_delegation_branches();
    for (const auto& branch : branches) {
        run_git(repo_dir_, "branch -D " + branch);
    }
    if (!branches.empty()) {
        logger->info("Pruned {} stale delegation branch(es)",
                     branches.size());
    }
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
 * @version 2.0.6
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
        logger->warn("create_worktree failed, cleaning stale state: {}",
                     r.output);
        run_git(repo_dir_, "worktree remove --force '"
                           + path.string() + "'");
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        run_git(repo_dir_, "branch -D " + branch);
        r = run_git(repo_dir_, cmd);
        if (!r.success) {
            logger->error("create_worktree failed after cleanup: {}",
                          r.output);
            return std::nullopt;
        }
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
/**
 * @brief Remove worktree directory and delete branch.
 *
 * Uses `git worktree remove --force` first, then
 * `std::filesystem::remove_all` as a belt-and-suspenders fallback —
 * if git fails silently (locked fd, permissions), the directory is
 * still removed by the filesystem call.
 *
 * @param info Worktree to clean up.
 * @internal
 * @version 2.0.6
 */
void WorktreeManager::cleanup(const WorktreeInfo& info) {
    run_git(repo_dir_, "worktree remove --force '" +
                       info.path.string() + "'");
    std::error_code ec;
    std::filesystem::remove_all(info.path, ec);
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
