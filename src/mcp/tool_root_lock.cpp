// SPDX-License-Identifier: Apache-2.0
/**
 * @file tool_root_lock.cpp
 * @brief ToolRootLock implementation (gh#158, gh#160).
 * @version 2.13.0
 */

#include <entropic/mcp/tool_root_lock.h>
#include <entropic/types/logging.h>

static auto logger = entropic::log::get("mcp.tool_root_lock");

namespace entropic {

/**
 * @brief Acquire exclusively; re-entrant for the owning thread.
 *
 * Only the owner ever stores its own id into `owner_`, so a non-owner can
 * never read its own id there and always takes the mutex — which is what
 * makes the unlocked `owner_` check sound.
 *
 * @req REQ-LOOP-009
 * @req REQ-DELEG-005
 * @version 2.13.0
 */
void ToolRootLock::lock() {
    const auto me = std::this_thread::get_id();
    if (owner_.load(std::memory_order_acquire) == me) {
        ++depth_;
        return;
    }
    mutex_.lock();
    owner_.store(me, std::memory_order_release);
    depth_ = 1;
}

/**
 * @brief Release one exclusive level; the last one frees the lock.
 * @req REQ-LOOP-009
 * @req REQ-DELEG-005
 * @version 2.13.0
 */
void ToolRootLock::unlock() {
    if (--depth_ == 0) {
        owner_.store(std::thread::id{}, std::memory_order_release);
        mutex_.unlock();
    }
}

/**
 * @brief Acquire shared, unless this thread owns the exclusive side.
 *
 * The owner passing through is the point, not a shortcut: a sandboxed
 * child's own tool calls run on the thread that entered the sandbox, and
 * they are the calls that must see it. Anyone else who finds the lock
 * held logs the wait before and after, so a stalled dispatch is visible.
 *
 * @param what Label for the wait log line.
 * @return true when a shared hold was taken; false for the owner.
 * @req REQ-LOOP-009
 * @req REQ-DELEG-005
 * @version 2.13.0
 */
bool ToolRootLock::lock_shared(const std::string& what) {
    if (owned_by_this_thread()) {
        return false;
    }
    if (!mutex_.try_lock_shared()) {
        logger->info("[{}] waiting: a sandboxed delegation has this server "
                     "set re-rooted; dispatching once it restores", what);
        mutex_.lock_shared();
        logger->info("[{}] working directory restored — dispatching", what);
    }
    return true;
}

/**
 * @brief Release a shared hold taken by lock_shared() == true.
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
void ToolRootLock::unlock_shared() {
    mutex_.unlock_shared();
}

/**
 * @brief Whether the calling thread holds the exclusive side.
 * @return true for the owner, false for every other thread.
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
bool ToolRootLock::owned_by_this_thread() const {
    return owner_.load(std::memory_order_acquire)
        == std::this_thread::get_id();
}

/**
 * @brief Take the shared side (or pass through as the owner).
 * @param lock Lock to hold.
 * @param what Label for the wait log line.
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
ToolRootShared::ToolRootShared(ToolRootLock& lock, const std::string& what)
    : lock_(lock), held_(lock.lock_shared(what)) {}

/**
 * @brief Release the shared hold if one was taken.
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
ToolRootShared::~ToolRootShared() {
    if (held_) {
        lock_.unlock_shared();
    }
}

} // namespace entropic
