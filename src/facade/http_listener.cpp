// SPDX-License-Identifier: Apache-2.0
/**
 * @file http_listener.cpp
 * @brief Streamable-HTTP front end for the external bridge (gh#204).
 *
 * See http_listener.h for the transport and the security bar.
 *
 * @version 2.14.0
 */

#include "http_listener.h"

#include <entropic/mcp/external_bridge.h>
#include <entropic/types/logging.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace entropic::http {

namespace {

auto logger = log::get("mcp.http_listener");

constexpr size_t kTokenBytes = 32;                  ///< 256 bits
constexpr size_t kMaxBodyBytes = 4u * 1024 * 1024;  ///< One JSON-RPC message
constexpr int kKeepaliveMs = 15000;                 ///< SSE idle comment
constexpr int kThrottleFailures = 5;                ///< Failures that block
constexpr auto kThrottleWindow = std::chrono::seconds(60);
constexpr auto kThrottleBlock = std::chrono::seconds(30);

/**
 * @brief A JSON body with one error member, for refusals.
 * @param message Human-readable reason.
 * @return Serialized `{"error": message}`.
 * @utility
 * @version 2.14.0
 */
std::string error_body(const std::string& message) {
    return nlohmann::json{{"error", message}}.dump();
}

}  // namespace

/**
 * @brief A fresh bearer token: 256 bits from getrandom(), hex-encoded.
 * @return 64 lowercase hex characters, or "" if the kernel gave no entropy.
 * @utility
 * @version 2.14.0
 */
std::string generate_token() {
    unsigned char buf[kTokenBytes];
    size_t got = 0;
    while (got < kTokenBytes) {
        const ssize_t n = ::getrandom(buf + got, kTokenBytes - got, 0);
        if (n <= 0 && errno != EINTR) { return ""; }
        got += n > 0 ? static_cast<size_t>(n) : 0;
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(kTokenBytes * 2);
    for (unsigned char b : buf) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0f]);
    }
    return out;
}

/**
 * @brief Compare two tokens in time independent of where they differ.
 * @param a First value.
 * @param b Second value.
 * @return true when equal.
 * @utility
 * @version 2.14.0
 */
bool tokens_equal(const std::string& a, const std::string& b) {
    // The token's length is public (always 64), so leaking a length
    // mismatch through the early-out gives nothing away.
    if (a.size() != b.size() || a.empty()) { return false; }
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

/**
 * @brief Whether a Host header names this listener's loopback endpoint.
 * @param host Host header value ("" when absent).
 * @param port Listening port.
 * @return true for `127.0.0.1:<port>` or `localhost:<port>`.
 * @utility
 * @version 2.14.0
 */
bool host_allowed(const std::string& host, int port) {
    const std::string suffix = ":" + std::to_string(port);
    return host == "127.0.0.1" + suffix || host == "localhost" + suffix;
}

/**
 * @brief The token in an `Authorization: Bearer <t>` header, or "".
 * @param header Authorization header value.
 * @return The token, or "" when the scheme is not Bearer.
 * @utility
 * @version 2.14.0
 */
std::string bearer_token(const std::string& header) {
    static const std::string scheme = "Bearer ";
    return header.rfind(scheme, 0) == 0 ? header.substr(scheme.size())
                                        : std::string{};
}

/**
 * @brief Where the token for a port is written.
 * @param port Listening port.
 * @return `$HOME/.entropic/serve/<port>.token`.
 * @utility
 * @version 2.14.0
 */
std::filesystem::path token_path_for(int port) {
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = home != nullptr ? home : "/tmp";
    return base / ".entropic" / "serve" / (std::to_string(port) + ".token");
}

/**
 * @brief Create or verify the token directory: 0700, ours, not a symlink.
 * @param dir Directory to prepare.
 * @param[out] err Why it is unusable.
 * @return true when the directory is safe to hold a token.
 * @utility
 * @version 2.14.0
 */
bool prepare_private_dir(const std::filesystem::path& dir, std::string& err) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0) {
        err = dir.string() + ": " + std::strerror(errno);
    } else if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        err = dir.string() + " is not a plain directory";
    } else if (st.st_uid != ::getuid()) {
        err = dir.string() + " is owned by another user";
    } else if ((st.st_mode & 0777) != 0700 && ::chmod(dir.c_str(), 0700) != 0) {
        err = dir.string() + ": cannot set mode 0700";
    }
    return err.empty();
}

/**
 * @brief Write `token` to `path` atomically (temp + rename), mode 0600.
 * @param path Destination.
 * @param token Token text.
 * @param[out] err Why the write failed.
 * @return true on success.
 * @utility
 * @version 2.14.0
 */
bool write_token_file(const std::filesystem::path& path,
                      const std::string& token, std::string& err) {
    const std::string tmp = path.string() + ".tmp." + std::to_string(::getpid());
    const int fd = ::open(tmp.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          0600);
    bool ok = fd >= 0
        && ::write(fd, token.data(), token.size())
               == static_cast<ssize_t>(token.size())
        && ::fsync(fd) == 0;
    if (fd >= 0) { ::close(fd); }
    ok = ok && ::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) {
        err = "cannot write " + path.string() + ": " + std::strerror(errno);
        ::unlink(tmp.c_str());
    }
    return ok;
}

/**
 * @brief Whether requests are refused at `now`.
 * @param now Current time.
 * @return true while a block is in force.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
bool AuthThrottle::blocked(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    return now < blocked_until_;
}

/**
 * @brief Record one failed authentication at `now`.
 * @param now Current time.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
void AuthThrottle::record_failure(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (now - window_start_ > kThrottleWindow) {
        window_start_ = now;
        failures_ = 0;
    }
    if (++failures_ >= kThrottleFailures) {
        blocked_until_ = now + kThrottleBlock;
        failures_ = 0;
    }
}

// ── Server ──────────────────────────────────────────────────

/// @brief httplib state, kept out of the header (no third-party headers).
struct HttpListener::Impl {
    httplib::Server server;  ///< The HTTP server
    std::string token;       ///< This run's bearer token
    AuthThrottle throttle;   ///< Repeated-failure block
    std::atomic<bool> listen_returned{false};  ///< Listen thread has exited
};

namespace {

/**
 * @brief One streamed tools/call: a dispatching thread and the pipe it
 *        writes to (gh#204).
 *
 * The bridge's dispatch() writes progress to an fd exactly as it does for a
 * unix-socket client, so a socketpair lets HTTP reuse it unchanged. The
 * reader drains the pipe to EOF even after the HTTP client has gone, so the
 * dispatching thread can never block on a full pipe and is always joinable.
 *
 * @version 2.14.0
 */
struct SseJob {
    int read_fd = -1;     ///< Our end
    std::thread worker;   ///< Runs dispatch()

    /**
     * @brief Join the dispatching thread, then close our end of the pipe.
     * @req REQ-BRIDGE-001
     * @version 2.14.0
     */
    ~SseJob() {
        if (worker.joinable()) { worker.join(); }
        if (read_fd >= 0) { ::close(read_fd); }
    }
};

/**
 * @brief Forward complete lines from `buf` as SSE events; keep the rest.
 * @param buf Accumulated bytes; consumed lines are removed.
 * @param sink Response sink.
 * @param live Cleared when the client stops accepting writes.
 * @utility
 * @version 2.14.0
 */
void forward_lines(std::string& buf, httplib::DataSink& sink, bool& live) {
    size_t nl = 0;
    while ((nl = buf.find('\n')) != std::string::npos) {
        const std::string event = "data: " + buf.substr(0, nl) + "\n\n";
        live = live && sink.write(event.data(), event.size());
        buf.erase(0, nl + 1);
    }
}

/**
 * @brief Pump the pipe into the SSE response until the worker closes it.
 * @param fd Read end.
 * @param sink Response sink.
 * @utility
 * @version 2.14.0
 */
void pump_sse(int fd, httplib::DataSink& sink) {
    std::string buf;
    bool live = true;
    char chunk[4096];
    for (;;) {
        struct pollfd pfd{fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, kKeepaliveMs);
        if (ready == 0) {
            // A long tool call can go quiet; a comment keeps proxies and
            // clients from treating the stream as dead.
            static const char ka[] = ": keepalive\n\n";
            live = live && sink.write(ka, sizeof(ka) - 1);
            continue;
        }
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) { break; }  // EOF: dispatch finished and closed its end
        buf.append(chunk, static_cast<size_t>(n));
        forward_lines(buf, sink, live);
    }
    if (live) { sink.done(); }
}

/**
 * @brief Answer a tools/call as an SSE stream: progress, then the result.
 * @param bridge Bridge to dispatch to.
 * @param body JSON-RPC request.
 * @param res Response.
 * @utility
 * @version 2.14.0
 */
void stream_call(ExternalBridge* bridge, const std::string& body,
                 httplib::Response& res) {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        res.status = 500;
        res.set_content(error_body("socketpair failed"), "application/json");
        return;
    }
    auto job = std::make_shared<SseJob>();
    job->read_fd = sv[0];
    job->worker = std::thread([bridge, body, fd = sv[1]] {
        const std::string reply = bridge->dispatch(body, fd) + "\n";
        (void)::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
        ::close(fd);
    });
    res.set_header("Cache-Control", "no-cache");
    res.set_chunked_content_provider(
        "text/event-stream",
        [job](size_t, httplib::DataSink& sink) {
            pump_sse(job->read_fd, sink);
            return true;
        });
}

/**
 * @brief Refuse a request that fails the security bar, or return 0.
 * @param req Request.
 * @param port Bound port.
 * @param impl Token and throttle.
 * @return HTTP status to refuse with, or 0 to proceed.
 * @utility
 * @version 2.14.0
 */
int refusal_status(const httplib::Request& req, int port,
                   HttpListener::Impl& impl) {
    const auto now = AuthThrottle::Clock::now();
    int status = 0;
    if (req.has_header("Origin")) {
        status = 403;  // a browser page — never ours to answer
    } else if (!host_allowed(req.get_header_value("Host"), port)) {
        status = 403;  // DNS rebinding
    } else if (impl.throttle.blocked(now)) {
        status = 429;
    } else if (!tokens_equal(bearer_token(req.get_header_value("Authorization")),
                             impl.token)) {
        impl.throttle.record_failure(now);
        status = 401;
    }
    if (status != 0) {
        logger->warn("[http] refused {} {} from {}: {}", req.method, req.path,
                     req.remote_addr, status);  // never the token
    }
    return status;
}

/**
 * @brief Handle POST /mcp after the security gate.
 * @param bridge Bridge to dispatch to.
 * @param req Request.
 * @param res Response.
 * @utility
 * @version 2.14.0
 */
void handle_post(ExternalBridge* bridge, const httplib::Request& req,
                 httplib::Response& res) {
    const auto msg = nlohmann::json::parse(req.body, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) {
        res.status = 400;  // includes JSON-RPC batches, removed in 2025-06-18
        res.set_content(error_body("expected one JSON-RPC message"),
                        "application/json");
    } else if (!msg.contains("id") || !msg.contains("method")) {
        res.status = 202;  // a notification or a response: nothing to return
    } else if (msg.value("method", "") == "tools/call") {
        stream_call(bridge, req.body, res);
    } else {
        res.set_content(bridge->dispatch(req.body, -1), "application/json");
    }
}

}  // namespace

/**
 * @brief Bind to 127.0.0.1:`port` on start(); 0 picks a free port.
 * @param bridge Bridge to dispatch to (not owned).
 * @param port Port; 0 = any free port (tests).
 * @version 2.14.0
 */
HttpListener::HttpListener(ExternalBridge* bridge, int port)
    : impl_(std::make_unique<Impl>()), bridge_(bridge), port_(port) {}

/**
 * @brief Stop the server and remove the token file.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
HttpListener::~HttpListener() { stop(); }

/**
 * @brief Install routes, limits and the worker pool on the server.
 * @param impl Server state.
 * @param bridge Bridge to dispatch to.
 * @param port Bound port (for the Host check).
 * @utility
 * @version 2.14.0
 */
static void configure_server(HttpListener::Impl& impl, ExternalBridge* bridge,
                             const std::atomic<int>& port) {
    auto& s = impl.server;
    s.set_payload_max_length(kMaxBodyBytes);
    s.set_read_timeout(30, 0);
    s.set_write_timeout(30, 0);
    s.new_task_queue = [] { return new httplib::ThreadPool(32); };
    s.Post("/mcp", [&impl, bridge, &port](const httplib::Request& req,
                                          httplib::Response& res) {
        const int refuse = refusal_status(req, port.load(), impl);
        if (refuse != 0) {
            res.status = refuse;
            res.set_content(error_body("refused"), "application/json");
            return;
        }
        handle_post(bridge, req, res);
    });
    auto not_allowed = [](const httplib::Request&, httplib::Response& res) {
        res.status = 405;  // no server-initiated stream, no transport session
    };
    s.Get("/mcp", not_allowed);
    s.Delete("/mcp", not_allowed);
}

/**
 * @brief Bind 127.0.0.1 on `port`, or on any free port when it is 0.
 * @param server The server.
 * @param port Requested port.
 * @return The bound port, or <= 0 on failure.
 * @utility
 * @version 2.14.0
 */
static int bind_loopback(httplib::Server& server, int port) {
    return port == 0 ? server.bind_to_any_port("127.0.0.1")
                     : (server.bind_to_port("127.0.0.1", port) ? port : -1);
}

/**
 * @brief Generate this run's token and write it where clients read it.
 * @param bound Bound port (names the token file).
 * @return "" on success, else why it failed.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
std::string HttpListener::publish_token(int bound) {
    std::string err;
    impl_->token = generate_token();
    if (impl_->token.empty()) {
        err = "no entropy for the bearer token";
    } else {
        token_file_ = token_path_for(bound);
        if (prepare_private_dir(token_file_.parent_path(), err)) {
            write_token_file(token_file_, impl_->token, err);
        }
    }
    return err;
}

/**
 * @brief Write the token, bind, and start serving.
 * @param[out] err Why it could not start.
 * @return true when listening.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
bool HttpListener::start(std::string& err) {
    configure_server(*impl_, bridge_, port_);
    const int bound = bind_loopback(impl_->server, port_.load());
    if (bound <= 0) {
        err = "cannot bind 127.0.0.1:" + std::to_string(port_.load());
    } else {
        port_ = bound;
        err = publish_token(bound);
    }
    if (err.empty()) {
        thread_ = std::thread([this] {
            impl_->server.listen_after_bind();
            impl_->listen_returned = true;
        });
        err = await_running();
    }
    if (err.empty()) {
        logger->info("[http] serving MCP at http://127.0.0.1:{}/mcp "
                     "(token: {})", bound, token_file_.string());
    }
    return err.empty();
}

/**
 * @brief Return only once the server is accepting, or say why not.
 *
 * httplib's Server::stop() is a no-op until its listen loop has set
 * is_running — so a stop() landing between thread start and that flag left
 * the loop running forever and the join hung. It surfaced as the gh#204 test
 * hanging only under ctest's 8-way parallel load, where the thread starts
 * slower. httplib's own wait_until_ready() would spin forever if listening
 * failed first, so this waits a bounded time and also watches for the thread
 * exiting.
 *
 * @return "" when running, else why it is not.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
std::string HttpListener::await_running() {
    for (int i = 0; i < 5000; ++i) {
        if (impl_->server.is_running() || impl_->listen_returned) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return impl_->server.is_running() ? std::string{}
                                      : std::string("listener did not start");
}

/**
 * @brief Stop serving and remove the token file. Idempotent.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
void HttpListener::stop() {
    impl_->server.stop();
    if (thread_.joinable()) { thread_.join(); }
    if (!token_file_.empty()) {
        std::error_code ec;
        std::filesystem::remove(token_file_, ec);
        token_file_.clear();
    }
}

}  // namespace entropic::http
