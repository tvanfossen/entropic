// SPDX-License-Identifier: Apache-2.0
/**
 * @file session_scoped.h
 * @brief Server state that belongs to a SESSION, not to the server (gh#158).
 *
 * @par Why
 * `concurrent_sessions` defaults true, so every unbound session on a handle
 * runs against ONE default server set. A built-in server that keeps
 * conversation-describing state on itself — the filesystem server's
 * read-before-write tracker, the entropic server's todo list — therefore
 * pooled it across sessions: session B could overwrite a file only session
 * A had read, and B's todo result listed A's items. That is the shape
 * decision #66 fixed for the ConstitutionalValidator's per-turn fields —
 * per-handle storage of a per-conversation thing — and locking it (46d3474,
 * 095a2f2) made it safe without making it right.
 *
 * @par The key
 * `current_run_session()`: the session the dispatching ToolExecutor routed
 * the call on, which it publishes to its own thread around every dispatch
 * (a run is a blocking call on its thread, so the value is unambiguous —
 * the same argument as `RunCancelScope`, decision #66). A delegated child
 * inherits its parent's key, so a child's read legitimately satisfies its
 * parent's write. Outside a run the key is `""`, the default session.
 *
 * @par Lifetime
 * An entry lives until the conversation it describes ends: the facade
 * releases it on `entropic_session_drop`, `entropic_session_context_clear`,
 * `entropic_session_context_set` and `entropic_context_clear`, through
 * `ServerManager::release_session` and the `SessionStateOwner` interface
 * below. Without that the map would grow by one entry per session a
 * long-running host ever served.
 *
 * @par Why not a virtual on MCPServerBase
 * Its vtable is part of the plugin ABI (`i_mcp_server.h`); a server that
 * owns per-session state opts in by also deriving `SessionStateOwner`, and
 * the manager finds it with a cross-cast — the base class does not change.
 *
 * @version 2.13.0
 */

#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace entropic {

/**
 * @brief One `T` per session key, under one leaf mutex (gh#158).
 *
 * Every accessor takes the lock for its whole body and calls nothing but
 * the supplied functor under it, so it is a LEAF lock: nothing that holds
 * it waits on another. A functor must not re-enter the same instance.
 *
 * @tparam T Per-session state (default-constructible).
 * @version 2.13.0
 */
template <typename T>
class SessionScoped {
public:
    /**
     * @brief Run `fn` on `key`'s state, creating it empty on first use.
     * @param key Session key.
     * @param fn Callable taking `T&`.
     * @return Whatever `fn` returns.
     * @req REQ-LOOP-009
     * @version 2.13.0
     */
    template <typename Fn>
    auto with(const std::string& key, Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn(by_session_[key]);
    }

    /**
     * @brief Run `fn` on `key`'s state WITHOUT creating it.
     *
     * A lookup must not allocate an entry, or merely asking "did this
     * session read X?" would grow the map for sessions that never read.
     *
     * @param key Session key.
     * @param fn Callable taking `const T*` (nullptr when absent).
     * @return Whatever `fn` returns.
     * @req REQ-LOOP-009
     * @version 2.13.0
     */
    template <typename Fn>
    auto peek(const std::string& key, Fn&& fn) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = by_session_.find(key);
        return fn(it == by_session_.end() ? nullptr : &it->second);
    }

    /**
     * @brief Forget `key`'s state.
     * @param key Session key.
     * @return true when an entry existed and was erased.
     * @req REQ-LOOP-009
     * @version 2.13.0
     */
    bool release(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return by_session_.erase(key) > 0;
    }

    /**
     * @brief Sessions currently holding state (bounded-growth probe).
     * @return Entry count.
     * @req REQ-LOOP-009
     * @version 2.13.0
     */
    std::size_t session_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return by_session_.size();
    }

private:
    mutable std::mutex mutex_;                     ///< Leaf lock (gh#158)
    std::unordered_map<std::string, T> by_session_; ///< key → state
};

/**
 * @brief A server that keeps state per session and can release it (gh#158).
 *
 * Mixed into a concrete server BESIDE MCPServerBase, so the base's
 * plugin-visible vtable is untouched. `ServerManager::release_session`
 * cross-casts each in-process server to this and releases the key.
 *
 * @version 2.13.0
 */
class SessionStateOwner {
public:
    /**
     * @brief Virtual destructor for a polymorphic mixin.
     * @version 2.13.0
     */
    virtual ~SessionStateOwner() = default;

    /**
     * @brief Drop everything this server holds for `key`.
     * @param key Session key ("" = the default session).
     * @return true when anything was released.
     * @req REQ-LOOP-009
     * @version 2.13.0
     */
    virtual bool release_session(const std::string& key) = 0;

    /**
     * @brief Sessions this server currently holds state for.
     * @return Session count (the largest of its per-session maps).
     * @req REQ-LOOP-009
     * @version 2.13.0
     */
    virtual std::size_t session_count() const = 0;
};

}  // namespace entropic
