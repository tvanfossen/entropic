// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file sandbox.h
 * @brief Filesystem-based sandbox isolation for delegations.
 *
 * Replaces the v1.8.6–v2.1.4 git-worktree-based isolation
 * (`WorktreeManager`) which corrupted user repo state (gh#29).
 *
 * The sandbox lives at `~/.entropic/sandbox/<session-id>/`, entirely
 * outside the user's project directory. The engine NEVER touches the
 * user's repo: no `git checkout`, no branches, no commits, no merges.
 * Delegations run in isolated copies of the project tree and produce a
 * portable unified-diff patch as their final artifact. The consumer
 * (TUI/CLI/IDE plugin) is responsible for applying that patch — with
 * the user's consent and authorship — back to the project.
 *
 * @par Layout
 * @code
 * ~/.entropic/sandbox/<session-id>/
 *   base/                       Project snapshot at session start
 *   d-<delegation-id>/          Per-delegation sandbox (copy of base/
 *                               or of a prior delegation when chaining)
 *   pending/<delegation-id>.patch
 *                               Default-deny output when the consumer
 *                               registered no completion callback.
 * @endcode
 *
 * @par Invariant
 * Every filesystem write performed by this module must occur at a path
 * under `session_base_`. The `path_in_session_base()` helper is the
 * single chokepoint enforcing this; tests assert it directly.
 *
 * @version 2.1.5
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Identifies one delegation's sandbox directory.
 * @version 2.1.5
 */
struct SandboxInfo {
    std::filesystem::path path;       ///< Sandbox directory (under session base)
    std::string delegation_id;        ///< Short delegation id (e.g. "d1", "pipeline")
    std::filesystem::path base_dir;   ///< Snapshot used as this sandbox's starting state
};

/**
 * @brief Final artifact emitted by a finalized sandbox.
 *
 * `patch` is a unified diff (output of `git diff --no-index base/ head/`)
 * suitable for `git apply` in any tree. `files_touched` lists files that
 * differ between base and head, relative to the sandbox root. The caller
 * (typically the facade) forwards this to the consumer via the
 * delegation-complete callback.
 *
 * @version 2.1.5
 */
struct SandboxResult {
    std::string patch;                                ///< Unified diff text
    std::vector<std::filesystem::path> files_touched; ///< Relative paths that changed
    std::filesystem::path base_dir;                   ///< Snapshot the diff is against
    std::filesystem::path head_dir;                   ///< Final sandbox state
};

/**
 * @brief Create, finalize, and discard per-delegation filesystem sandboxes.
 *
 * One instance per engine session. Owns `~/.entropic/sandbox/<session-id>/`
 * and removes it in the destructor. Creates an initial `base/` snapshot
 * of the project (filtered by `.gitignore` when the project is a git
 * repo) on the first `create_sandbox()` call. Subsequent delegations
 * either branch from `base/` or chain from a prior delegation's output.
 *
 * @par Threading
 * Not thread-safe. The caller (`DelegationManager`) serializes access.
 *
 * @par Example
 * @code
 * SandboxManager mgr(project_dir);
 * auto s1 = mgr.create_sandbox("d1", std::nullopt);
 * // ... agent edits files in s1->path ...
 * auto r1 = mgr.finalize_sandbox(*s1);  // emits patch, removes s1->path
 *
 * auto s2 = mgr.create_sandbox("d2", s1);  // chains from d1 head
 * // ... agent edits in s2->path ...
 * auto r2 = mgr.finalize_sandbox(*s2);
 * @endcode
 *
 * @version 2.1.5
 */
class SandboxManager {
public:
    /**
     * @brief Construct with the user's project directory.
     *
     * Generates `<pid>-<hex8>` session id and reserves
     * `~/.entropic/sandbox/<session_id>/`. The base snapshot is not
     * created until the first `create_sandbox()` call — sessions that
     * never delegate pay no snapshot cost.
     *
     * Prunes at startup: session directories under `~/.entropic/sandbox/`
     * whose owner pid is no longer alive.
     *
     * @param project_dir Path to the user's project root (used as
     *                    snapshot source; never written to by this class).
     * @version 2.1.5
     */
    explicit SandboxManager(const std::filesystem::path& project_dir);

    /**
     * @brief Remove this session's sandbox tree.
     *
     * Best-effort: errors are logged, not thrown. Path containment is
     * enforced — refuses to remove anything outside `session_base_`.
     *
     * @version 2.1.5
     */
    ~SandboxManager();

    SandboxManager(SandboxManager&&) = default;
    SandboxManager& operator=(SandboxManager&&) = default;
    SandboxManager(const SandboxManager&) = delete;
    SandboxManager& operator=(const SandboxManager&) = delete;

    /**
     * @brief Create a new delegation sandbox.
     *
     * On the first call of a session, materializes the `base/` snapshot
     * from the project directory, honoring `.gitignore` (via
     * `git ls-files --cached --others --exclude-standard`) when the
     * project is a git repository. For non-git projects, the entire
     * project tree is copied verbatim.
     *
     * The new sandbox is a copy of either `base/` (when `chain_from` is
     * empty) or `chain_from->path` (for pipeline forward-carry).
     *
     * @param delegation_id Short, filesystem-safe id (e.g. "d1", "d2",
     *                      "pipeline"). Becomes the sandbox dir name.
     * @param chain_from    Optional prior sandbox to chain from. When
     *                      set, the new sandbox starts from that
     *                      sandbox's current state instead of `base/`.
     * @return SandboxInfo on success, `std::nullopt` if snapshot or
     *         copy failed (logged at ERROR).
     * @version 2.1.5
     */
    std::optional<SandboxInfo> create_sandbox(
        const std::string& delegation_id,
        std::optional<SandboxInfo> chain_from = std::nullopt);

    /**
     * @brief Produce the final patch artifact for a sandbox.
     *
     * Runs `git diff --no-index --binary <base_dir>/ <sandbox>/` to
     * generate a portable unified diff, collects the list of changed
     * files, and returns them in a `SandboxResult`. The sandbox
     * directory is NOT removed by this call — the caller decides when
     * to discard (see `discard_sandbox()`), allowing the patch consumer
     * to inspect files before cleanup.
     *
     * @param info Sandbox to finalize.
     * @return SandboxResult on success, `std::nullopt` on failure.
     * @version 2.1.5
     */
    std::optional<SandboxResult> finalize_sandbox(const SandboxInfo& info);

    /**
     * @brief Remove a sandbox directory.
     *
     * Path-containment guarded — refuses to remove anything outside
     * `session_base_`. Safe to call multiple times.
     *
     * @param info Sandbox to remove.
     * @version 2.1.5
     */
    void discard_sandbox(const SandboxInfo& info);

    /**
     * @brief Get the project directory this manager snapshots from.
     * @return Project root path.
     * @version 2.1.5
     */
    const std::filesystem::path& project_dir() const;

    /**
     * @brief Get this session's sandbox base directory.
     * @return Path to `~/.entropic/sandbox/<session-id>/`.
     * @version 2.1.5
     */
    const std::filesystem::path& session_base() const;

private:
    /**
     * @brief Ensure the `base/` snapshot exists.
     * @return true on success, false on failure (logged).
     * @version 2.1.5
     */
    bool ensure_base_snapshot();

    /**
     * @brief Copy `source` tree into `target`.
     *
     * For a git project source, uses `git ls-files` to enumerate tracked
     * and untracked-but-not-ignored files. For non-git sources or for
     * chaining copies (sandbox-to-sandbox), uses a recursive copy.
     *
     * @param source Source directory.
     * @param target Destination directory (must be inside session_base_).
     * @return true on success.
     * @version 2.1.5
     */
    bool snapshot_tree(const std::filesystem::path& source,
                       const std::filesystem::path& target);

    /**
     * @brief Path containment check.
     *
     * The single chokepoint enforcing the invariant that all writes
     * occur under `session_base_`. Uses `lexically_normal()` and a
     * relative-path check (not substring) to avoid bypasses like
     * `.../sandbox-backup/...`.
     *
     * @param p Path to check.
     * @return true if `p` is lexically inside `session_base_`.
     * @version 2.1.5
     */
    bool path_in_session_base(const std::filesystem::path& p) const;

    /**
     * @brief Recursive remove guarded by `path_in_session_base()`.
     * @param p Path to remove.
     * @version 2.1.5
     */
    void safe_remove(const std::filesystem::path& p);

    /**
     * @brief Remove dead-session sandbox dirs from `~/.entropic/sandbox/`.
     * @version 2.1.5
     */
    void prune_stale_sessions();

    std::filesystem::path project_dir_;   ///< User's project root
    std::string session_id_;              ///< `<pid>-<hex8>`
    std::filesystem::path session_base_;  ///< `~/.entropic/sandbox/<session_id>/`
    std::filesystem::path base_dir_;      ///< `session_base_/base`
    bool base_ready_ = false;             ///< `base/` snapshot exists
};

/**
 * @brief RAII directory swapper for sandbox-scoped tool execution.
 *
 * Swaps the engine's tool working directory to the sandbox path on
 * construction and restores it on destruction. Used by
 * `DelegationManager` to route MCP server (filesystem, bash, …) calls
 * into the sandbox during a delegation.
 *
 * @version 2.1.5
 */
class ScopedSandbox {
public:
    /**
     * @brief Callback type for directory swapping.
     *
     * The facade implements this to iterate registered servers and
     * call `entropic_mcp_server_set_working_dir()` on each.
     *
     * @param path New working directory.
     * @param user_data Opaque pointer (facade context).
     * @version 2.1.5
     */
    using SwapDirFn = void (*)(
        const std::filesystem::path& path, void* user_data);

    /**
     * @brief Construct and swap directories.
     * @param swap_fn       Directory swap callback (may be null — no-op).
     * @param user_data     Opaque pointer for `swap_fn`.
     * @param sandbox_path  Target sandbox directory.
     * @param original_path Original directory to restore on destruction.
     * @version 2.1.5
     */
    ScopedSandbox(SwapDirFn swap_fn,
                  void* user_data,
                  const std::filesystem::path& sandbox_path,
                  const std::filesystem::path& original_path);

    /**
     * @brief Restore the original directory.
     * @version 2.1.5
     */
    ~ScopedSandbox();

    ScopedSandbox(const ScopedSandbox&) = delete;
    ScopedSandbox& operator=(const ScopedSandbox&) = delete;

private:
    SwapDirFn swap_fn_;                   ///< Directory swap callback
    void* user_data_;                     ///< Opaque pointer
    std::filesystem::path original_path_; ///< Path to restore
};

} // namespace entropic
