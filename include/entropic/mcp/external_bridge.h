// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file external_bridge.h
 * @brief Unix socket MCP bridge — exposes a running engine to external clients.
 *
 * When mcp.external.enabled is true, the engine starts an ExternalBridge
 * that listens on a unix domain socket and serves JSON-RPC 2.0 (MCP
 * protocol) to external clients such as Claude Code. The bridge operates
 * on the already-configured engine handle — no second engine instance.
 *
 * @par Exposed tools:
 * - entropic.ask — submit a prompt, get the full response
 * - entropic.status — engine version + message count
 * - entropic.context_clear — reset conversation
 * - entropic.context_count — message count
 *
 * @par Socket path:
 * Uses ExternalMCPConfig.socket_path if set, otherwise derived from
 * the project directory via compute_socket_path().
 *
 * @par Thread safety:
 * The bridge runs a background accept loop. Each connected client is
 * served sequentially (one request at a time). The engine handle's
 * api_mutex serializes access to the engine.
 *
 * @version 2.0.8
 */

#pragma once

#include <entropic/entropic_export.h>
#include <entropic/types/config.h>

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// Forward declaration — bridge holds a raw pointer to the opaque handle.
struct entropic_engine;
using entropic_handle_t = entropic_engine*;

namespace entropic {

/**
 * @brief Unix socket MCP bridge for external client access.
 *
 * Listens on a unix domain socket and dispatches JSON-RPC requests
 * to the engine handle it was constructed with. Lifetime is tied
 * to the engine handle — created in configure_common, destroyed
 * before engine teardown.
 *
 * @internal
 * @version 2.0.6-rc16.2
 */
class ENTROPIC_EXPORT ExternalBridge {
public:
    /**
     * @brief Construct with engine handle and config.
     * @param handle Engine handle (must outlive the bridge).
     * @param config External MCP configuration.
     * @param project_dir Project directory (for socket path derivation).
     * @version 2.0.8
     */
    ExternalBridge(
        entropic_handle_t handle,
        const ExternalMCPConfig& config,
        const std::filesystem::path& project_dir);

    ~ExternalBridge();

    /**
     * @brief Start the background accept loop.
     * @return true if the socket was created and listening.
     * @version 2.0.8
     */
    bool start();

    /**
     * @brief Stop the accept loop and close the socket.
     * @version 2.0.8
     */
    void stop();

    /**
     * @brief Get the socket path (for logging/diagnostics).
     * @return Socket path.
     * @utility
     * @version 2.0.8
     */
    const std::filesystem::path& socket_path() const { return socket_path_; }

    /**
     * @brief Handle entropic.ask_status — check async task state.
     * @param args Tool arguments (JSON with task_id).
     * @return MCP tool result JSON.
     * @internal
     * @version 2.0.11
     */
    nlohmann::json handle_ask_status(const nlohmann::json& args);

    /**
     * @brief Run an async entropic.ask in a background thread.
     * @param prompt User prompt.
     * @param task_id Assigned task ID.
     * @param client_fd Socket fd for completion notification.
     * @internal
     * @version 2.0.11
     */
    /**
     * @brief Install VERIFYING-state observer scoped to one task.
     *
     * Registers a state-change observer on the handle and records
     * task_id as the active task. The observer maps VERIFYING
     * transitions onto phase="validating" (first) or "revising"
     * (subsequent). Pair with detach_phase_observer().
     * (P1-5 follow-up, 2.0.6-rc16.2)
     *
     * @param task_id Task whose phase will be updated.
     * @internal
     * @version 2.0.6-rc16.2
     */
    void attach_phase_observer(const std::string& task_id);

    /**
     * @brief Clear the phase observer installed by attach_phase_observer.
     * @internal
     * @version 2.0.6-rc16.2
     */
    void detach_phase_observer();

    void run_async_ask(const std::string& prompt,
                       const std::string& task_id,
                       int client_fd);

    /**
     * @brief Remove tasks older than TTL from the registry.
     * @internal
     * @version 2.0.11
     */
    void cleanup_expired_tasks();

    /// @brief Async task mutex (public for dispatch_tool access).
    mutable std::mutex tasks_mutex_;

    /**
     * @brief Async task state for background entropic.ask runs.
     *
     * Moved to public section in 2.0.6-rc16 so cancel-on-clear can
     * flip task statuses with the mutex held. (P1-5, P1-8)
     *
     * @internal
     * @version 2.0.6-rc16
     */
    struct AsyncTask {
        std::string status = "queued";   ///< queued | running | done | error | cancelled (2.0.6-rc16)
        std::string phase = "queued";    ///< queued, running, running:<tier>, done, failed, cancelled (P1-5)
        std::string result;              ///< Final text or error message
        std::chrono::steady_clock::time_point created; ///< For TTL cleanup
    };

    /**
     * @brief Mutable accessor to the task registry.
     *
     * Caller MUST hold tasks_mutex_. Used by the cancel-on-clear
     * path to flip task statuses atomically with the mutex held.
     * (P1-8, 2.0.6-rc16)
     *
     * @return Reference to task registry.
     * @utility
     * @version 2.0.6-rc16
     */
    std::unordered_map<std::string, AsyncTask>& tasks_for_cancel() {
        return tasks_;
    }

    /**
     * @brief Return the currently-active async task_id (if any).
     * @return Task id or empty string. Caller MUST hold tasks_mutex_.
     * @utility
     * @version 2.0.6-rc16.2
     */
    const std::string& active_task_id_for_observer() const {
        return active_task_id_;
    }

    /**
     * @brief Add a connected fd to the subscriber set.
     * @param fd Connected client socket fd.
     * @internal (public for unit test access)
     * @version 2.0.6-rc16
     */
    void subscribe(int fd);

    /**
     * @brief Remove an fd from the subscriber set.
     * @param fd Client socket fd being closed.
     * @internal (public for unit test access)
     * @version 2.0.6-rc16
     */
    void unsubscribe(int fd);

    /**
     * @brief Write a JSON-RPC notification to every subscribed fd.
     *
     * Writes are serialized under subscribers_mutex_ so concurrent
     * broadcasts do not interleave on the same fd. Fds that fail to
     * write are removed from the set.
     *
     * @param notif JSON-RPC notification object.
     * @internal (public for unit test access)
     * @version 2.0.6-rc16
     */
    void broadcast_notification(const nlohmann::json& notif);

    /**
     * @brief Current subscriber count (for diagnostics / testing).
     * @return Number of connected clients currently subscribed.
     * @utility
     * @version 2.0.6-rc16
     */
    size_t subscriber_count() const {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        return subscribers_.size();
    }

    /**
     * @brief Update status/phase for a tracked task atomically.
     *
     * Used by the async run thread to advance through queued →
     * running → running:<tier> → done|failed|cancelled. Safe to call
     * with an unknown task_id (no-op).
     *
     * @param task_id Task identifier.
     * @param status New coarse status string.
     * @param phase New granular phase string.
     * @utility
     * @version 2.0.6-rc16
     */
    void update_task_phase(const std::string& task_id,
                           const std::string& status,
                           const std::string& phase);

private:
    /**
     * @brief Background thread: accept connections and serve.
     * @internal
     * @version 2.0.8
     */
    void accept_loop();

    /**
     * @brief Serve a single connected client until disconnect.
     * @param client_fd Connected socket file descriptor.
     * @internal
     * @version 2.0.8
     */
    void serve_client(int client_fd);

    /**
     * @brief Dispatch a JSON-RPC request and return the response.
     * @param request Raw JSON-RPC request string.
     * @param client_fd Socket fd for streaming progress notifications.
     * @return JSON-RPC response string, or empty for notifications.
     * @internal
     * @version 2.0.10
     */
    std::string dispatch(const std::string& request, int client_fd);

    entropic_handle_t handle_;                 ///< Engine handle (not owned)
    ExternalMCPConfig config_;                 ///< Config snapshot
    std::filesystem::path socket_path_;        ///< Unix socket path
    int listen_fd_ = -1;                       ///< Listening socket fd
    std::atomic<bool> running_{false};         ///< Accept loop running
    std::thread accept_thread_;                ///< Background accept thread

    /// @brief Async task registry (task_id → state). Guarded by tasks_mutex_.
    std::unordered_map<std::string, AsyncTask> tasks_;

    /// @brief Subscribed client fds for broadcast notifications.
    /// @details Populated on connect, drained on disconnect. Guarded by
    /// subscribers_mutex_. Replaces the v2.0.11 single-fd scheme so TUI
    /// and Claude Code can both receive ask_complete / progress events
    /// simultaneously. (P0-2, 2.0.6-rc16)
    std::unordered_set<int> subscribers_;
    mutable std::mutex subscribers_mutex_; ///< Guards subscribers_

    /// @brief Currently-executing async task_id (empty if none).
    /// Set by run_async_ask before entropic_run and cleared on exit.
    /// Read by the state-change observer so VERIFYING transitions can
    /// be projected onto the right task's phase. Guarded by tasks_mutex_.
    /// (P1-5 follow-up, 2.0.6-rc16.2)
    std::string active_task_id_;
};

} // namespace entropic
