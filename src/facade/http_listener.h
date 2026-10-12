// SPDX-License-Identifier: Apache-2.0
/**
 * @file http_listener.h
 * @brief Serve the external MCP bridge over Streamable HTTP (gh#204, v2.14.0).
 *
 * @par Why
 * Every MCP client that wanted the engine spawned its own
 * `entropic mcp-bridge` over stdio, and a consumer started a host per client
 * behind it — N editors, N hosts, N × idle device memory. MCP's answer to
 * "many clients, one server" is a network transport: every client points at
 * one URL. This serves the SAME bridge — the same tools, sessions and turn
 * queue the unix socket serves — at `http://127.0.0.1:<port>/mcp`.
 *
 * @par Security (decision #84)
 * The listener never leaves loopback and carries no TLS: remote use goes
 * through `ssh -L <port>:127.0.0.1:<port> host`, which supplies both. Over TCP
 * there is no SO_PEERCRED, so a bearer token stands in for it:
 *   - 256 bits from getrandom(), regenerated every start, written atomically
 *     to `~/.entropic/serve/<port>.token`, mode 0600, in a directory that must
 *     be 0700, owned by this uid and not a symlink — otherwise start refuses.
 *     Removed on a clean stop.
 *   - Compared in constant time; never logged, never echoed.
 *   - Any request carrying an `Origin` header is refused (a browser page
 *     cannot drive the engine), as is a `Host` that is not this loopback
 *     address and port (DNS rebinding).
 *   - Body and header sizes capped; read/write timeouts; repeated auth
 *     failures throttled, each logged without the token.
 *
 * @par Transport
 * POST /mcp carries one JSON-RPC message. A notification or response gets
 * 202. `tools/call` streams: progress notifications, then the result, as SSE
 * events. Anything else answers as `application/json`. GET and DELETE are 405:
 * there is no server-initiated stream and no transport session (conversation
 * identity is the `session` tool argument). Async completion notifications
 * are delivered over the unix socket only; an HTTP client polls `ask_status`.
 *
 * Exported (ENTROPIC_EXPORT) for white-box tests only, as ExternalBridge
 * is; not part of the C API.
 *
 * @version 2.14.0
 */
#pragma once

#include <entropic/entropic_export.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace entropic {

class ExternalBridge;

namespace http {

/**
 * @brief A fresh bearer token: 256 bits from getrandom(), hex-encoded.
 * @return 64 lowercase hex characters, or "" if the kernel gave no entropy.
 * @utility
 * @version 2.14.0
 */
ENTROPIC_EXPORT std::string generate_token();

/**
 * @brief Compare two tokens in time independent of where they differ.
 * @param a First value.
 * @param b Second value.
 * @return true when equal.
 * @utility
 * @version 2.14.0
 */
ENTROPIC_EXPORT bool tokens_equal(const std::string& a, const std::string& b);

/**
 * @brief Whether a Host header names this listener's loopback endpoint.
 * @param host Host header value ("" when absent).
 * @param port Listening port.
 * @return true for `127.0.0.1:<port>` or `localhost:<port>`.
 * @utility
 * @version 2.14.0
 */
ENTROPIC_EXPORT bool host_allowed(const std::string& host, int port);

/**
 * @brief The token in an `Authorization: Bearer <t>` header, or "".
 * @param header Authorization header value.
 * @return The token, or "" when the scheme is not Bearer.
 * @utility
 * @version 2.14.0
 */
ENTROPIC_EXPORT std::string bearer_token(const std::string& header);

/**
 * @brief Where the token for a port is written.
 * @param port Listening port.
 * @return `$HOME/.entropic/serve/<port>.token`.
 * @utility
 * @version 2.14.0
 */
ENTROPIC_EXPORT std::filesystem::path token_path_for(int port);

/**
 * @brief Create or verify the token directory: 0700, ours, not a symlink.
 * @param dir Directory to prepare.
 * @param[out] err Why it is unusable.
 * @return true when the directory is safe to hold a token.
 * @utility
 * @version 2.14.0
 */
ENTROPIC_EXPORT bool prepare_private_dir(const std::filesystem::path& dir, std::string& err);

/**
 * @brief Write `token` to `path` atomically (temp + rename), mode 0600.
 * @param path Destination.
 * @param token Token text.
 * @param[out] err Why the write failed.
 * @return true on success.
 * @utility
 * @version 2.14.0
 */
ENTROPIC_EXPORT bool write_token_file(const std::filesystem::path& path,
                      const std::string& token, std::string& err);

/**
 * @brief Refuses after repeated authentication failures (decision #84).
 *
 * Five failures inside one minute block every request for thirty seconds.
 * Thread-safe; the clock is passed in so the policy is testable.
 *
 * @dg_internal
 * @version 2.14.0
 */
class ENTROPIC_EXPORT AuthThrottle {
public:
    using Clock = std::chrono::steady_clock;

    /**
     * @brief Whether requests are refused at `now`.
     * @param now Current time.
     * @return true while a block is in force.
     * @version 2.14.0
     */
    bool blocked(Clock::time_point now) const;

    /**
     * @brief Record one failed authentication at `now`.
     * @param now Current time.
     * @version 2.14.0
     */
    void record_failure(Clock::time_point now);

private:
    mutable std::mutex mu_;
    int failures_ = 0;                 ///< Failures in the current window
    Clock::time_point window_start_{}; ///< When the window opened
    Clock::time_point blocked_until_{};///< Block expiry
};

/**
 * @brief Streamable-HTTP front end for an ExternalBridge (gh#204).
 *
 * Owns the listening socket, the server thread and the token file. Does not
 * own the bridge, which must outlive it.
 *
 * @dg_internal
 * @version 2.14.0
 */
class ENTROPIC_EXPORT HttpListener {
public:
    /**
     * @brief Bind to 127.0.0.1:`port` on start(); 0 picks a free port.
     * @param bridge Bridge to dispatch to (not owned).
     * @param port Port; 0 = any free port (tests).
     * @version 2.14.0
     */
    HttpListener(ExternalBridge* bridge, int port);

    /// @brief Stops the server and removes the token file.
    ~HttpListener();

    HttpListener(const HttpListener&) = delete;
    HttpListener& operator=(const HttpListener&) = delete;

    /**
     * @brief Write the token, bind, and start serving.
     * @param[out] err Why it could not start.
     * @return true when listening.
     * @version 2.14.0
     */
    bool start(std::string& err);

    /**
     * @brief Stop serving and remove the token file. Idempotent.
     * @version 2.14.0
     */
    void stop();

    /**
     * @brief The port actually bound.
     * @return Port, or 0 before start().
     * @version 2.14.0
     */
    int port() const { return port_.load(); }

    /**
     * @brief Where this listener's token was written.
     * @return Token file path (empty before start()).
     * @version 2.14.0
     */
    const std::filesystem::path& token_file() const { return token_file_; }

    /// @brief Opaque server state (httplib stays out of this header).
    struct Impl;

private:
    /**
     * @brief Generate this run's token and write it where clients read it.
     * @param bound Bound port (names the token file).
     * @return "" on success, else why it failed.
     * @version 2.14.0
     */
    std::string publish_token(int bound);

    /**
     * @brief Return only once the server is accepting, or say why not.
     * @return "" when running, else why it is not.
     * @version 2.14.0
     */
    std::string await_running();

    std::unique_ptr<Impl> impl_;          ///< httplib server + state
    ExternalBridge* bridge_;              ///< Not owned
    std::atomic<int> port_{0};            ///< Bound port
    std::filesystem::path token_file_;    ///< Written token path
    std::thread thread_;                  ///< listen_after_bind thread
};

}  // namespace http
}  // namespace entropic
