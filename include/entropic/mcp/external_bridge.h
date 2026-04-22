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
 * @version 2.0.11
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

    /**
     * @brief Async task state for background entropic.ask runs.
     * @internal
     * @version 2.0.11
     */
    struct AsyncTask {
        std::string status = "running";        ///< "running" | "done" | "error"
        std::string result;                    ///< Final text or error message
        std::chrono::steady_clock::time_point created; ///< For TTL cleanup
    };

    entropic_handle_t handle_;                 ///< Engine handle (not owned)
    ExternalMCPConfig config_;                 ///< Config snapshot
    std::filesystem::path socket_path_;        ///< Unix socket path
    int listen_fd_ = -1;                       ///< Listening socket fd
    std::atomic<bool> running_{false};         ///< Accept loop running
    std::thread accept_thread_;                ///< Background accept thread

    /// @brief Async task registry (task_id → state). Guarded by tasks_mutex_.
    std::unordered_map<std::string, AsyncTask> tasks_;
    /// @brief Active client fd for async notifications (-1 if none).
    std::atomic<int> active_client_fd_{-1};
};

} // namespace entropic
