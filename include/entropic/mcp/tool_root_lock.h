// SPDX-License-Identifier: Apache-2.0
/**
 * @file tool_root_lock.h
 * @brief The lock that guards the working directory one ServerManager's
 *        in-process and plugin servers resolve against (gh#158, gh#160).
 *
 * @par Why this exists
 * `FilesystemServer`, `BashServer` and `GitServer` hold ONE working
 * directory each, and a sandboxed delegation (gh#160) re-points all of
 * them at its sandbox for the child's whole run. With `concurrent_sessions`
 * on (the v2.13.0 default) a second, unbound session dispatches on the SAME
 * default server set at the same moment, so without this lock its
 * `read_file("notes.md")` resolved inside the OTHER session's sandbox —
 * and the swap itself was an unsynchronized write of a `std::filesystem::
 * path` (and an ignore-rule reload) racing that read.
 *
 * @par Contract
 * - EXCLUSIVE: taken by a thread that re-roots the servers, and held from
 *   sandbox entry to restore. Re-entrant for the owning thread, because a
 *   nested delegation re-enters on the same thread.
 * - SHARED: taken around every in-process / plugin tool dispatch. The
 *   exclusive owner passes straight through — its own child's calls are
 *   exactly the ones that SHOULD see the sandbox.
 *
 * So another session's dispatch WAITS while a sandbox is in place, and
 * then resolves against its own root. The parallel alternative is a
 * workspace per session (gh#166), which has its own server set and its own
 * lock.
 *
 * @par Lock order
 * Outermost of the generation-path locks: `ToolRootLock` (exclusive, held
 * across a child's run) → `generation_mutex_` → `swap_mutex_` /
 * `records_mutex_` → `transition_mutex_` / `mtp_mutex_`. The SHARED side
 * is taken only at tool dispatch, where a run thread holds none of those,
 * and nothing inside a dispatch takes a generate entry point — so no
 * holder of an inner lock ever waits on this one.
 *
 * @version 2.13.0
 */

#pragma once

#include <entropic/entropic_export.h>

#include <atomic>
#include <shared_mutex>
#include <string>
#include <thread>

namespace entropic {

/**
 * @brief Shared/exclusive lock whose exclusive side is re-entrant for its
 *        owner, and whose owner passes through the shared side.
 *
 * Satisfies BasicLockable (`lock` / `unlock`), so `std::lock_guard` works
 * for the exclusive side; `ToolRootShared` is the RAII shared side.
 *
 * @return n/a (class).
 * @req REQ-LOOP-009
 * @req REQ-DELEG-005
 * @version 2.13.0
 */
class ENTROPIC_EXPORT ToolRootLock {
public:
    /**
     * @brief Acquire exclusively; re-entrant for the owning thread.
     * @version 2.13.0
     */
    void lock();

    /**
     * @brief Release one exclusive level; the last one frees the lock.
     * @version 2.13.0
     */
    void unlock();

    /**
     * @brief Acquire shared, unless this thread owns the exclusive side.
     *
     * A dispatch that has to wait says so in the log, naming what waited:
     * a tool call stalled behind another session's sandboxed delegation is
     * otherwise indistinguishable from a slow tool.
     *
     * @param what Label for the wait log line (the tool being dispatched).
     * @return true when a shared hold was taken and must be released with
     *         unlock_shared(); false when this thread is the exclusive
     *         owner, which already excludes every re-rooting.
     * @version 2.13.0
     */
    bool lock_shared(const std::string& what);

    /**
     * @brief Release a shared hold taken by lock_shared() == true.
     * @version 2.13.0
     */
    void unlock_shared();

    /**
     * @brief Whether the calling thread holds the exclusive side.
     * @return true for the owner, false for every other thread.
     * @version 2.13.0
     */
    bool owned_by_this_thread() const;

private:
    std::shared_mutex mutex_;                ///< The lock itself
    std::atomic<std::thread::id> owner_{};   ///< Exclusive owner, or none
    int depth_ = 0;                          ///< Owner's re-entry depth
};

/**
 * @brief RAII shared hold on a ToolRootLock (owner pass-through aware).
 * @return An RAII guard holding the shared side (or passing through as the
 *         exclusive owner) until it goes out of scope.
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
class ENTROPIC_EXPORT ToolRootShared {
public:
    /**
     * @brief Take the shared side (or pass through as the owner).
     * @param lock Lock to hold.
     * @param what Label for the wait log line (the tool being dispatched).
     * @version 2.13.0
     */
    ToolRootShared(ToolRootLock& lock, const std::string& what);

    /**
     * @brief Release the shared hold if one was taken.
     * @version 2.13.0
     */
    ~ToolRootShared();

    ToolRootShared(const ToolRootShared&) = delete;
    ToolRootShared& operator=(const ToolRootShared&) = delete;

private:
    ToolRootLock& lock_;  ///< Held lock
    bool held_;           ///< Whether a shared hold was taken
};

} // namespace entropic
