// SPDX-License-Identifier: Apache-2.0
/**
 * @file sandbox.cpp
 * @brief SandboxManager and ScopedSandbox implementation.
 *
 * Replaces the v1.8.6–v2.1.4 git-worktree manager that corrupted the
 * user's repo state (gh#29). All filesystem operations live under
 * `~/.entropic/sandbox/<session-id>/`; the user's project directory is
 * read but never written.
 *
 * @version 2.1.5
 */

#include <entropic/core/sandbox.h>
#include <entropic/types/logging.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>

#include <pwd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static auto logger = entropic::log::get("core.sandbox");

namespace entropic {

namespace {

/**
 * @brief Quote a path for safe inclusion in a shell command.
 * @param p Path to quote.
 * @return Single-quoted shell-escaped string.
 * @utility
 * @version 2.1.5
 */
std::string shell_quote(const std::filesystem::path& p) {
    std::string s = p.string();
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

/**
 * @brief Run a shell command and capture combined stdout+stderr.
 * @param cmd Full shell command.
 * @param[out] output Captured output.
 * @return Exit status (0 on success).
 * @utility
 * @version 2.1.5
 */
int run_capture(const std::string& cmd, std::string& output) {
    output.clear();
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return -1;
    }
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        output += buf.data();
    }
    return pclose(pipe);
}

/**
 * @brief Resolve `~/.entropic/sandbox/` honoring `$HOME`.
 * @return Sandbox root path.
 * @utility
 * @version 2.1.5
 */
std::filesystem::path sandbox_root() {
    const char* home = std::getenv("HOME");
    if ((home == nullptr) || home[0] == '\0') {
        struct passwd* pw = getpwuid(getuid());
        home = (pw != nullptr) ? pw->pw_dir : "/tmp";
    }
    return std::filesystem::path(home) / ".entropic" / "sandbox";
}

/**
 * @brief Generate `<pid>-<hex8>` session id.
 * @return Session identifier.
 * @utility
 * @version 2.1.5
 */
std::string make_session_id() {
    std::random_device rd;
    std::uniform_int_distribution<uint32_t> dist;
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", dist(rd));
    return std::to_string(static_cast<long>(getpid())) + "-" + hex;
}

/**
 * @brief Check whether `pid` is still alive (or not ours to touch).
 *
 * Returns false only on `ESRCH` (process is gone). `EPERM` means the
 * process exists but is owned by another uid — we leave it alone.
 *
 * @param pid Process id.
 * @return true if alive or not ours.
 * @utility
 * @version 2.1.5
 */
bool pid_is_alive(long pid) {
    if (kill(static_cast<pid_t>(pid), 0) == 0) { return true; }
    return errno != ESRCH;
}

/**
 * @brief Parse `<pid>-<hex8>` session-id directory name.
 * @param name Directory basename.
 * @param[out] pid Extracted pid on success.
 * @return true if `name` matches the session-id pattern.
 * @utility
 * @version 2.1.5
 */
bool parse_session_pid(const std::string& name, long& pid) {
    auto dash = name.find('-');
    bool shape_ok = dash != std::string::npos && dash != 0 &&
                    name.size() - dash - 1 == 8;
    if (!shape_ok) { return false; }
    bool parsed = false;
    try {
        size_t consumed = 0;
        pid = std::stol(name.substr(0, dash), &consumed);
        parsed = (consumed == dash);
    } catch (const std::exception&) {
        parsed = false;
    }
    return parsed;
}

/**
 * @brief Return true when `dir` is the root of a git repository.
 * @param dir Directory to inspect.
 * @return true if `dir/.git` exists (file or directory).
 * @utility
 * @version 2.1.5
 */
bool is_git_repo(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir / ".git", ec);
}

} // namespace

// ── SandboxManager ───────────────────────────────────────

/**
 * @brief Construct with the user's project directory.
 * @param project_dir Project root (read-only to this class).
 * @internal
 * @version 2.1.5
 */
SandboxManager::SandboxManager(const std::filesystem::path& project_dir)
    : project_dir_(std::filesystem::absolute(project_dir)),
      session_id_(make_session_id()),
      session_base_(sandbox_root() / session_id_),
      base_dir_(session_base_ / "base") {
    logger->info("SandboxManager: project={} session={} base={}",
                 project_dir_.string(), session_id_,
                 session_base_.string());
    prune_stale_sessions();
    std::error_code ec;
    std::filesystem::create_directories(session_base_, ec);
    if (ec) {
        logger->error("Failed to create session base {}: {}",
                      session_base_.string(), ec.message());
    }
}

/**
 * @brief Destructor — remove this session's sandbox tree.
 * @internal
 * @version 2.1.5
 */
SandboxManager::~SandboxManager() {
    safe_remove(session_base_);
    logger->info("Session sandbox cleanup: {}", session_base_.string());
}

/**
 * @brief Path containment check — single chokepoint for write safety.
 *
 * Uses `lexically_normal()` then `lexically_relative()` to verify the
 * normalized path is inside `session_base_`. Substring matching is
 * explicitly avoided (a `…/sandbox-backup/…` path would pass a
 * substring check).
 *
 * @param p Path to check.
 * @return true if `p` is inside `session_base_`.
 * @internal
 * @version 2.1.5
 */
bool SandboxManager::path_in_session_base(
    const std::filesystem::path& p) const {
    if (session_base_.empty() || p.empty()) { return false; }
    auto norm = std::filesystem::absolute(p).lexically_normal();
    auto base = session_base_.lexically_normal();
    auto rel = norm.lexically_relative(base);
    if (rel.empty()) { return false; }
    auto first = rel.begin();
    return first != rel.end() && first->string() != "..";
}

/**
 * @brief Recursive remove guarded by `path_in_session_base()`.
 * @param p Path to remove.
 * @internal
 * @version 2.1.5
 */
void SandboxManager::safe_remove(const std::filesystem::path& p) {
    if (p.empty()) { return; }
    if (p == session_base_) {
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        if (ec) {
            logger->warn("remove_all({}) failed: {}",
                         p.string(), ec.message());
        }
        return;
    }
    if (!path_in_session_base(p)) {
        logger->error("BLOCKED remove outside session base: path='{}' "
                      "session_base='{}'",
                      p.string(), session_base_.string());
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    if (ec) {
        logger->warn("remove_all({}) failed: {}", p.string(), ec.message());
    }
}

/**
 * @brief Remove dead-session sandbox dirs from `~/.entropic/sandbox/`.
 * @internal
 * @version 2.1.5
 */
void SandboxManager::prune_stale_sessions() {
    auto root = sandbox_root();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) { return; }
    for (const auto& entry :
         std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_directory()) { continue; }
        std::string name = entry.path().filename().string();
        if (name == session_id_) { continue; }
        long pid = 0;
        if (!parse_session_pid(name, pid)) { continue; }
        if (pid_is_alive(pid)) { continue; }
        std::error_code rec;
        std::filesystem::remove_all(entry.path(), rec);
        logger->info("Pruned stale session sandbox: {} (pid {} gone)",
                     entry.path().string(), pid);
    }
}

/**
 * @brief Ensure the `base/` snapshot of the project exists.
 * @return true on success.
 * @internal
 * @version 2.1.5
 */
bool SandboxManager::ensure_base_snapshot() {
    if (base_ready_) { return true; }
    std::error_code ec;
    std::filesystem::create_directories(base_dir_, ec);
    bool ok = !ec && snapshot_tree(project_dir_, base_dir_);
    if (!ok) {
        logger->error("Failed to materialize base snapshot at {}: {}",
                      base_dir_.string(),
                      ec ? ec.message() : std::string{"snapshot_tree failed"});
        return false;
    }
    base_ready_ = true;
    logger->info("Base snapshot ready at {}", base_dir_.string());
    return true;
}

/**
 * @brief Copy git-tracked + untracked-not-ignored files from a project.
 * @param source Git repository root.
 * @param target Destination directory.
 * @return true on success.
 * @utility
 * @version 2.1.5
 */
static bool snapshot_git_project(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
    std::string list_cmd = "cd " + shell_quote(source) +
        " && git ls-files --cached --others --exclude-standard";
    std::string out;
    if (run_capture(list_cmd, out) != 0) {
        logger->error("git ls-files failed in {}", source.string());
        return false;
    }
    std::istringstream iss(out);
    std::string rel;
    while (std::getline(iss, rel)) {
        if (rel.empty()) { continue; }
        auto src = source / rel;
        auto dst = target / rel;
        std::error_code ec;
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::filesystem::copy_file(
            src, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            logger->warn("copy {} -> {} failed: {}",
                         src.string(), dst.string(), ec.message());
        }
    }
    return true;
}

/**
 * @brief Recursive copy for non-git sources (or sandbox-to-sandbox chains).
 * @param source Source directory.
 * @param target Destination directory.
 * @return true on success.
 * @utility
 * @version 2.1.5
 */
static bool snapshot_plain_copy(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::copy(
        source, target,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing |
            std::filesystem::copy_options::copy_symlinks,
        ec);
    if (ec) {
        logger->error("plain copy {} -> {} failed: {}",
                      source.string(), target.string(), ec.message());
        return false;
    }
    return true;
}

/**
 * @brief Copy `source` tree into `target` honoring `.gitignore` when possible.
 * @param source Source directory.
 * @param target Destination (must be inside `session_base_`).
 * @return true on success.
 * @internal
 * @version 2.1.5
 */
bool SandboxManager::snapshot_tree(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
    if (!path_in_session_base(target)) {
        logger->error("BLOCKED snapshot to path outside session base: {}",
                      target.string());
        return false;
    }
    if (is_git_repo(source)) {
        return snapshot_git_project(source, target);
    }
    return snapshot_plain_copy(source, target);
}

/**
 * @brief Create a new delegation sandbox.
 * @param delegation_id Short id (becomes the sandbox dir name).
 * @param chain_from Optional prior sandbox to chain from.
 * @return SandboxInfo on success.
 * @internal
 * @version 2.1.5
 */
std::optional<SandboxInfo> SandboxManager::create_sandbox(
    const std::string& delegation_id,
    std::optional<SandboxInfo> chain_from) {
    auto target = session_base_ / ("d-" + delegation_id);
    auto source = chain_from.has_value() ? chain_from->path : base_dir_;
    auto effective_base = chain_from.has_value()
                              ? chain_from->base_dir
                              : base_dir_;

    bool path_ok = path_in_session_base(target);
    bool base_ok = ensure_base_snapshot();
    if (!path_ok || !base_ok) {
        if (!path_ok) {
            logger->error("BLOCKED sandbox path outside session base: {}",
                          target.string());
        }
        return std::nullopt;
    }

    std::error_code ec;
    std::filesystem::remove_all(target, ec);
    if (!snapshot_plain_copy(source, target)) {
        return std::nullopt;
    }

    logger->info("Created sandbox: id={} path={} chained_from={}",
                 delegation_id, target.string(),
                 chain_from.has_value() ? chain_from->path.string()
                                        : std::string{"(base)"});
    return SandboxInfo{target, delegation_id, effective_base};
}

/**
 * @brief Run `git diff --no-index` between base and head, capture patch.
 * @param base Snapshot directory.
 * @param head Final sandbox directory.
 * @param[out] patch Captured unified diff (may be empty when no changes).
 * @return true on success (zero or one exit code from git diff).
 * @utility
 * @version 2.1.5
 */
static bool capture_diff(
    const std::filesystem::path& base,
    const std::filesystem::path& head,
    std::string& patch) {
    std::string cmd = "git diff --no-index --binary --no-color "
                      + shell_quote(base) + " " + shell_quote(head);
    int status = run_capture(cmd, patch);
    if (status < 0) {
        logger->error("git diff failed to spawn");
        return false;
    }
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exit_code != 0 && exit_code != 1) {
        logger->error("git diff exit={} output snippet='{}'",
                      exit_code, patch.substr(0, 256));
        return false;
    }
    return true;
}

/**
 * @brief Strip a directory prefix `p` from the front of `line`.
 * @param[in,out] line Path line to trim in place.
 * @param p Prefix directory (a trailing slash is added if missing).
 * @utility
 * @version 2.3.7
 */
static void trim_path_prefix(std::string& line, std::string p) {
    if (!p.empty() && p.back() != '/') { p += '/'; }
    size_t n = 0;
    if (line.rfind(p, 0) == 0) {
        n = p.size();
    } else if (!p.empty() && p.front() == '/'
               && line.rfind(p.substr(1), 0) == 0) {
        n = p.size() - 1;
    }
    if (n > 0) { line.erase(0, n); }
}

/**
 * @brief List files differing between `base` and `head` via `diff -rq`-style logic.
 * @param base Snapshot directory.
 * @param head Final sandbox directory.
 * @return Vector of relative paths.
 * @utility
 * @version 2.3.7
 */
static std::vector<std::filesystem::path> diff_files(
    const std::filesystem::path& base,
    const std::filesystem::path& head) {
    std::vector<std::filesystem::path> changed;
    std::string cmd = "git diff --no-index --name-only "
                      + shell_quote(base) + " " + shell_quote(head);
    std::string out;
    run_capture(cmd, out);
    std::istringstream iss(out);
    std::string line;
    auto base_s = base.string();
    auto head_s = head.string();
    while (std::getline(iss, line)) {
        if (line.empty()) { continue; }
        trim_path_prefix(line, base_s);
        trim_path_prefix(line, head_s);
        changed.emplace_back(line);
    }
    return changed;
}

/**
 * @brief Produce the final patch artifact for a sandbox.
 * @param info Sandbox to finalize.
 * @return SandboxResult on success.
 * @internal
 * @version 2.1.5
 */
std::optional<SandboxResult> SandboxManager::finalize_sandbox(
    const SandboxInfo& info) {
    if (!path_in_session_base(info.path)) {
        logger->error("finalize_sandbox: path not in session base: {}",
                      info.path.string());
        return std::nullopt;
    }
    SandboxResult res;
    res.base_dir = info.base_dir;
    res.head_dir = info.path;
    if (!capture_diff(info.base_dir, info.path, res.patch)) {
        return std::nullopt;
    }
    res.files_touched = diff_files(info.base_dir, info.path);
    logger->info("Finalized sandbox {}: {} files changed, "
                 "patch={} bytes",
                 info.delegation_id, res.files_touched.size(),
                 res.patch.size());
    return res;
}

/**
 * @brief Remove a sandbox directory.
 * @param info Sandbox to remove.
 * @internal
 * @version 2.1.5
 */
void SandboxManager::discard_sandbox(const SandboxInfo& info) {
    safe_remove(info.path);
    logger->info("Discarded sandbox {}: {}",
                 info.delegation_id, info.path.string());
}

/**
 * @brief Persist a patch under the session's `pending/` directory.
 *
 * Default-deny fallback path (gh#29, v2.1.5). Used when no
 * delegation-complete callback is registered or the consumer returns
 * REJECT. Refuses to write to anything outside `session_base_`.
 *
 * @param delegation_id Short id (basename for the .patch file).
 * @param patch         Unified-diff text.
 * @return Path written, or `std::nullopt` on failure.
 * @internal
 * @version 2.1.5
 */
std::optional<std::filesystem::path> SandboxManager::write_pending_patch(
    const std::string& delegation_id,
    const std::string& patch) {
    auto pending_dir = session_base_ / "pending";
    auto out_path = pending_dir / (delegation_id + ".patch");
    std::error_code ec;
    bool contained = path_in_session_base(pending_dir)
                  && path_in_session_base(out_path);
    if (!contained) {
        logger->error("Refusing write_pending_patch: {} not in session base",
                      out_path.string());
        return std::nullopt;
    }
    std::filesystem::create_directories(pending_dir, ec);
    bool dir_ok = !ec;
    FILE* f = dir_ok ? std::fopen(out_path.c_str(), "wb") : nullptr;
    bool write_ok = false;
    if (f != nullptr) {
        write_ok = std::fwrite(patch.data(), 1, patch.size(), f)
                   == patch.size();
        std::fclose(f);
    }
    if (!dir_ok || f == nullptr || !write_ok) {
        logger->error("write_pending_patch failed for {} (mkdir={} open={} "
                      "write={})", out_path.string(), dir_ok,
                      f != nullptr, write_ok);
        return std::nullopt;
    }
    return out_path;
}

/**
 * @brief Get the project directory this manager snapshots from.
 * @return Project root path.
 * @internal
 * @version 2.1.5
 */
const std::filesystem::path& SandboxManager::project_dir() const {
    return project_dir_;
}

/**
 * @brief Get this session's sandbox base directory.
 * @return Path to `~/.entropic/sandbox/<session-id>/`.
 * @internal
 * @version 2.1.5
 */
const std::filesystem::path& SandboxManager::session_base() const {
    return session_base_;
}

// ── ScopedSandbox ────────────────────────────────────────

/**
 * @brief Construct and swap directories.
 * @param swap_fn       Directory swap callback.
 * @param user_data     Opaque pointer for `swap_fn`.
 * @param sandbox_path  Target sandbox directory.
 * @param original_path Original directory to restore.
 * @internal
 * @version 2.1.5
 */
ScopedSandbox::ScopedSandbox(
    SwapDirFn swap_fn,
    void* user_data,
    const std::filesystem::path& sandbox_path,
    const std::filesystem::path& original_path)
    : swap_fn_(swap_fn),
      user_data_(user_data),
      original_path_(original_path) {
    if (swap_fn_ != nullptr) {
        swap_fn_(sandbox_path, user_data_);
        logger->info("Swapped tool dir to sandbox: {}",
                     sandbox_path.string());
    }
}

/**
 * @brief Restore the original directory.
 * @internal
 * @version 2.1.5
 */
ScopedSandbox::~ScopedSandbox() {
    if (swap_fn_ != nullptr) {
        swap_fn_(original_path_, user_data_);
        logger->info("Restored tool dir to: {}", original_path_.string());
    }
}

} // namespace entropic
