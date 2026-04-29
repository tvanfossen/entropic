// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file external_bridge.cpp
 * @brief Unix socket MCP bridge implementation.
 *
 * Listens on a unix domain socket, accepts one client at a time, and
 * serves JSON-RPC 2.0 (MCP) dispatching to the engine handle.
 *
 * @version 2.0.8
 */

#include <entropic/mcp/external_bridge.h>
#include <entropic/entropic.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

using json = nlohmann::json;

namespace entropic {

static auto logger = entropic::log::get("mcp.external_bridge");

// Forward declaration
std::filesystem::path compute_socket_path(
    const std::filesystem::path& project_dir);

// ── JSON-RPC helpers ─────────────────────────────────────

/**
 * @brief Build a JSON-RPC success response.
 * @param id Request ID.
 * @param result Result payload.
 * @return Serialized JSON-RPC response.
 * @utility
 * @version 2.0.8
 */
static std::string rpc_ok(const json& id, const json& result) {
    json r = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    return r.dump();
}

/**
 * @brief Build a JSON-RPC error response.
 * @param id Request ID.
 * @param code Error code.
 * @param msg Error message.
 * @return Serialized JSON-RPC response.
 * @utility
 * @version 2.0.8
 */
static std::string rpc_err(const json& id, int code,
                           const std::string& msg) {
    json r = {{"jsonrpc", "2.0"}, {"id", id},
              {"error", {{"code", code}, {"message", msg}}}};
    return r.dump();
}

/**
 * @brief Wrap text in MCP tool result shape.
 * @param text Result text.
 * @return MCP content array JSON.
 * @utility
 * @version 2.0.8
 */
static json tool_text(const std::string& text) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})}};
}

// ── Tool definitions ─────────────────────────────────────

/**
 * @brief MCP tool definitions exposed by the bridge.
 * @return JSON array of tool schemas.
 * @utility
 * @version 2.0.11
 */
static json tool_definitions() {
    return json::array({
        {{"name", "entropic.ask"},
         {"description",
          "Submit a prompt to the running entropic engine. "
          "Set async=true to return immediately with a task_id; "
          "the engine pushes a notification when done."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"prompt", {{"type", "string"},
                            {"description", "User message"}}},
                {"async", {{"type", "boolean"},
                           {"description", "Run asynchronously"},
                           {"default", false}}}
            }},
            {"required", json::array({"prompt"})}
         }}},
        {{"name", "entropic.ask_status"},
         {"description",
          "Check status of an async entropic.ask task."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {{"task_id", {
                {"type", "string"},
                {"description", "Task ID from async ask"}
            }}}},
            {"required", json::array({"task_id"})}
         }}},
        {{"name", "entropic.status"},
         {"description", "Engine version and message count."},
         {"inputSchema", {{"type", "object"},
                          {"properties", json::object()}}}},
        {{"name", "entropic.context_clear"},
         {"description", "Clear conversation history."},
         {"inputSchema", {{"type", "object"},
                          {"properties", json::object()}}}},
        {{"name", "entropic.context_count"},
         {"description", "Return the message count."},
         {"inputSchema", {{"type", "object"},
                          {"properties", json::object()}}}},
    });
}

// ── Tool handlers ────────────────────────────────────────

/**
 * @brief Extract the last assistant message from a run result JSON.
 *
 * entropic_run returns a JSON array of messages. This extracts the
 * content of the last assistant-role message — the final synthesized
 * answer after all tool calls, delegation, and validation.
 *
 * @param result_json JSON array string from entropic_run.
 * @return Last assistant message content, or empty on parse failure.
 * @utility
 * @version 2.0.10
 */
static std::string extract_final_text(const char* result_json) {
    auto arr = json::parse(result_json, nullptr, false);
    if (!arr.is_array()) { return {}; }
    for (auto it = arr.rbegin(); it != arr.rend(); ++it) {
        if (it->value("role", "") == "assistant") {
            return it->value("content", "");
        }
    }
    return {};
}

/**
 * @brief Write a JSON-RPC line to a socket fd.
 * @param fd Socket file descriptor.
 * @param msg JSON object to serialize and write.
 * @utility
 * @version 2.0.10
 */
static void write_json_line(int fd, const json& msg) {
    auto s = msg.dump() + "\n";
    ::write(fd, s.c_str(), s.size());
}

/**
 * @brief Send an MCP progress notification with a text token.
 * @param fd Client socket fd.
 * @param token_text Token string to send.
 * @param progress_token Progress token ID for correlation.
 * @utility
 * @version 2.0.10
 */
static void send_progress(int fd, const std::string& token_text,
                          const std::string& progress_token) {
    json notif = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/progress"},
        {"params", {
            {"progressToken", progress_token},
            {"progress", token_text}
        }}
    };
    write_json_line(fd, notif);
}

/**
 * @brief Handle entropic.ask — stream tokens then return final text.
 *
 * Uses entropic_run_streaming to send MCP progress notifications
 * for each token (streaming to client), then extracts the final
 * clean assistant text from the conversation for the tool result.
 *
 * @param handle Engine handle.
 * @param args Tool arguments.
 * @param client_fd Socket fd for streaming progress notifications.
 * @param call_id JSON-RPC request id (for progress token correlation).
 * @return MCP tool result JSON (final clean text).
 * @internal
 * @version 2.0.11
 */
static json handle_ask(entropic_handle_t handle, const json& args,
                       int client_fd, const std::string& call_id) {
    auto it = args.find("prompt");
    if (it == args.end() || !it->is_string()) {
        return tool_text("error: missing 'prompt' argument");
    }
    std::string prompt = it->get<std::string>();

    // Stream tokens as progress notifications
    struct StreamCtx { int fd; std::string token_id; };
    StreamCtx sctx{client_fd, call_id};
    auto on_token = [](const char* tok, size_t len, void* ud) {
        auto* ctx = static_cast<StreamCtx*>(ud);
        send_progress(ctx->fd, std::string(tok, len), ctx->token_id);
    };
    auto err = entropic_run_streaming(
        handle, prompt.c_str(), on_token, &sctx, nullptr);
    if (err != ENTROPIC_OK) {
        const char* msg = entropic_last_error(handle);
        return tool_text(std::string("error: ") + (msg ? msg : "unknown"));
    }

    // Extract clean final text from conversation
    char* msgs_json = nullptr;
    entropic_context_get(handle, &msgs_json);
    auto text = (msgs_json != nullptr)
        ? extract_final_text(msgs_json) : std::string{};
    entropic_free(msgs_json);
    return tool_text(text.empty() ? "(no response)" : text);
}

/**
 * @brief Handle entropic.status.
 * @param handle Engine handle.
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.6-rc16.2
 */
static json handle_status(entropic_handle_t handle) {
    size_t count = 0;
    entropic_context_count(handle, &count);
    std::ostringstream os;
    os << "entropic " << entropic_version()
       << "\nmessages: " << count;
    // Metrics + per-tier breakdown (P2-15 follow-up, 2.0.6-rc16.2)
    char* mjson = nullptr;
    if (entropic_metrics_json(handle, &mjson) == ENTROPIC_OK
        && mjson != nullptr) {
        os << "\nmetrics: " << mjson;
        entropic_free(mjson);
    }
    return tool_text(os.str());
}

/**
 * @brief Handle entropic.context_clear.
 * @param handle Engine handle.
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.8
 */
/**
 * @brief Cancel any async tasks currently running on the bridge.
 *
 * Marks tasks "cancelling", raises engine interrupt so the generation
 * loop unwinds, and polls up to ~1s for their worker threads to move
 * into a terminal state. The wait is best-effort — detached workers
 * may still be in flight when this returns, but the new context clear
 * will race them cleanly because each run owns its own result_json.
 *
 * @param handle Engine handle (for interrupt).
 * @param bridge Bridge whose tasks_ registry is being canceled.
 * @internal (P1-8, 2.0.6-rc16)
 * @version 2.0.6-rc16
 */
/**
 * @brief Final-state projection for an async entropic_run.
 *
 * Groups the three status/phase/text outputs so run_async_ask stays
 * under the 50-SLOC quality gate. (2.0.6-rc16)
 *
 * @internal
 */
struct AsyncFinalState {
    std::string status;   ///< done | error | cancelled
    std::string phase;    ///< done | failed | cancelled
    std::string text;     ///< result or error message
};

/**
 * @brief Translate entropic_run's return code into a final task state.
 *
 * On success, extracts the final clean assistant text from result_json
 * and frees it. On cancellation/error, reports the last-error message.
 * (P1-5, 2.0.6-rc16)
 *
 * @param handle Engine handle (for last-error lookup).
 * @param err Return code from entropic_run.
 * @param result_json JSON result (owned; freed on success, else NULL).
 * @return AsyncFinalState ready to be stored on the task.
 * @utility
 * @version 2.0.6-rc16
 */
static AsyncFinalState derive_async_final_state(
    entropic_handle_t handle,
    entropic_error_t err,
    char* result_json) {
    AsyncFinalState s;
    if (err == ENTROPIC_ERROR_CANCELLED
        || err == ENTROPIC_ERROR_INTERRUPTED) {
        const char* msg = entropic_last_error(handle);
        s.text = msg ? msg : "cancelled";
        s.status = "cancelled";
        s.phase = "cancelled";
    } else if (err != ENTROPIC_OK) {
        const char* msg = entropic_last_error(handle);
        s.text = msg ? msg : "unknown error";
        s.status = "error";
        s.phase = "failed";
    } else {
        s.text = extract_final_text(result_json);
        entropic_free(result_json);
        s.status = "done";
        s.phase = "done";
    }
    return s;
}

/**
 * @brief Mark every queued/running task as cancelling.
 * @param bridge Bridge holding the task registry.
 * @return true if any task was still in-flight.
 * @utility
 * @version 2.0.6-rc16
 */
static bool mark_tasks_cancelling(ExternalBridge* bridge) {
    std::lock_guard<std::mutex> lock(bridge->tasks_mutex_);
    bool any = false;
    for (auto& [_, task] : bridge->tasks_for_cancel()) {
        if (task.status == "queued" || task.status == "running") {
            task.status = "cancelled";
            task.phase = "cancelling";
            any = true;
        }
    }
    return any;
}

/**
 * @brief True while any task is still phase=cancelling.
 * @param bridge Bridge holding the task registry.
 * @utility
 * @version 2.0.6-rc16
 */
static bool any_cancelling_left(ExternalBridge* bridge) {
    std::lock_guard<std::mutex> lock(bridge->tasks_mutex_);
    for (auto& [_, task] : bridge->tasks_for_cancel()) {
        if (task.phase == "cancelling") { return true; }
    }
    return false;
}

/**
 * @brief Cancel any async tasks currently running on the bridge.
 *
 * Raises engine interrupt, flips still-running tasks into
 * phase="cancelling", and polls up to ~1s for detached workers to
 * finish. After the poll, unconditionally bumps the observer generation
 * via detach_phase_observer() so any post-poll callbacks from a still-
 * running worker are silently discarded. (P1-8, 2.0.6-rc16;
 * observer-gen force-detach: E5+E6, 2.1.0)
 *
 * @param handle Engine handle (for interrupt).
 * @param bridge Bridge whose tasks_ registry is being canceled.
 * @internal
 * @version 2.1.0
 */
static void cancel_inflight_async_tasks(
    entropic_handle_t handle, ExternalBridge* bridge) {
    if (bridge == nullptr) { return; }
    entropic_interrupt(handle);  // idempotent, cheap
    if (!mark_tasks_cancelling(bridge)) { return; }
    for (int i = 0; i < 20 && any_cancelling_left(bridge); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    bridge->detach_phase_observer();  // bump gen; silences any stale observer
}

/**
 * @brief entropic.context_clear MCP tool handler.
 *
 * Before clearing, cancels any in-flight async ask tasks so they do
 * not continue running against a stale context. (P1-8, 2.0.6-rc16)
 *
 * @param handle Engine handle.
 * @param bridge Bridge instance (used to reach the task registry).
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.6-rc16
 */
static json handle_clear(entropic_handle_t handle,
                         ExternalBridge* bridge) {
    cancel_inflight_async_tasks(handle, bridge);
    auto err = entropic_context_clear(handle);
    if (err != ENTROPIC_OK) {
        return tool_text("error: clear failed");
    }
    return tool_text("conversation cleared");
}

/**
 * @brief Handle entropic.context_count.
 * @param handle Engine handle.
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.8
 */
static json handle_count(entropic_handle_t handle) {
    size_t count = 0;
    entropic_context_count(handle, &count);
    return tool_text(std::to_string(count));
}

// ── Async ask status ─────────────────────────────────────

/**
 * @brief Handle entropic.ask_status — check async task state.
 *
 * Returns coarse status + granular phase so pollers see progression
 * through queued → running → running:<tier> → done|failed|cancelled.
 *
 * @param args Tool arguments (must contain "task_id").
 * @return MCP tool result JSON with status/phase/result/error.
 * @internal
 * @version 2.0.6-rc16
 */
json ExternalBridge::handle_ask_status(const json& args) {
    auto tid = args.value("task_id", std::string{});
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = tasks_.find(tid);
    if (it == tasks_.end()) {
        return tool_text("error: unknown task_id");
    }
    json status = {{"status", it->second.status},
                   {"phase",  it->second.phase}};
    if (!it->second.result.empty()) {
        auto key = (it->second.status == "error") ? "error" : "result";
        status[key] = it->second.result;
    }
    return tool_text(status.dump());
}

// ── UUID generation ──────────────────────────────────────

/**
 * @brief Generate a simple UUID-like task ID.
 * @return Hex string (16 chars from random_device + pid).
 * @utility
 * @version 2.0.11
 */
static std::string generate_task_id() {
    static std::atomic<uint64_t> counter{0};
    auto n = counter.fetch_add(1);
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream ss;
    ss << std::hex << (t ^ (n * 2654435761ULL));
    return "task-" + ss.str();
}

// ── Dispatch ─────────────────────────────────────────────

/**
 * @brief Route entropic.ask — sync (streaming) or async.
 * @param handle Engine handle.
 * @param bridge Bridge (for async task registry).
 * @param args Tool arguments.
 * @param client_fd Socket fd.
 * @param call_id Request id.
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.11
 */
static json dispatch_ask(entropic_handle_t handle,
                         ExternalBridge* bridge,
                         const json& args,
                         int client_fd,
                         const std::string& call_id) {
    if (args.value("async", false)) {
        auto task_id = generate_task_id();
        bridge->run_async_ask(
            args.value("prompt", ""), task_id, client_fd);
        return tool_text("async task started: " + task_id);
    }
    return handle_ask(handle, args, client_fd, call_id);
}

/**
 * @brief Dispatch a tools/call to the appropriate handler.
 *
 * entropic.ask streams progress notifications to client_fd during
 * generation, then returns the final clean text as the tool result.
 * When async=true, spawns a background thread and returns task_id.
 *
 * @param handle Engine handle.
 * @param bridge Bridge instance (for async task registry).
 * @param params JSON-RPC params (name + arguments).
 * @param client_fd Socket fd for streaming (entropic.ask only).
 * @param call_id JSON-RPC request id for progress correlation.
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.6-rc16
 */
static json dispatch_tool(entropic_handle_t handle,
                          ExternalBridge* bridge,
                          const json& params,
                          int client_fd,
                          const std::string& call_id) {
    std::string name = params.value("name", std::string{});
    json args = params.value("arguments", json::object());
    if (name == "entropic.ask") {
        return dispatch_ask(handle, bridge, args, client_fd, call_id);
    }
    json result;
    if      (name == "entropic.ask_status")    { result = bridge->handle_ask_status(args); }
    else if (name == "entropic.status")        { result = handle_status(handle); }
    else if (name == "entropic.context_clear") { result = handle_clear(handle, bridge); }
    else if (name == "entropic.context_count") { result = handle_count(handle); }
    else { result = tool_text("error: unknown tool '" + name + "'"); }
    return result;
}

// ── ExternalBridge ───────────────────────────────────────

/**
 * @brief Construct with engine handle and config.
 * @param handle Engine handle (must outlive the bridge).
 * @param config External MCP configuration.
 * @param project_dir Project directory (for socket path derivation).
 * @version 2.0.8
 */
ExternalBridge::ExternalBridge(
    entropic_handle_t handle,
    const ExternalMCPConfig& config,
    const std::filesystem::path& project_dir)
    : handle_(handle), config_(config) {
    socket_path_ = config.socket_path.has_value()
        ? config.socket_path.value()
        : compute_socket_path(project_dir);
}

/**
 * @brief Destructor — stop if running.
 * @version 2.0.8
 */
ExternalBridge::~ExternalBridge() {
    stop();
}

/**
 * @brief Create, bind, and listen on a unix domain socket.
 * @param path Socket filesystem path.
 * @return Listening fd, or -1 on failure.
 * @utility
 * @version 2.0.8
 */
static int create_listen_socket(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::remove(path);

    auto s = path.string();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    bool ok = (fd >= 0) && (s.size() < sizeof(sockaddr_un::sun_path));

    if (ok) {
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, s.c_str(),
                     sizeof(addr.sun_path) - 1);
        ok = (bind(fd, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) == 0)
          && (listen(fd, 1) == 0);
    }

    if (!ok) {
        logger->error("Socket setup failed for {}: {}",
                      s, std::strerror(errno));
        if (fd >= 0) { ::close(fd); }
        return -1;
    }
    return fd;
}

/**
 * @brief Start the background accept loop.
 * @return true if the socket was created and listening.
 * @internal
 * @version 2.0.8
 */
bool ExternalBridge::start() {
    listen_fd_ = create_listen_socket(socket_path_);
    if (listen_fd_ < 0) { return false; }

    running_.store(true);
    accept_thread_ = std::thread(&ExternalBridge::accept_loop, this);

    logger->info("External MCP bridge listening on {}",
                 socket_path_.string());
    return true;
}

/**
 * @brief Stop the accept loop, drain client threads, and close the socket.
 *
 * Issue #4 (v2.1.2, part D): pre-2.1.2 stop() only joined the accept
 * thread because serve_client ran inline. Now each connected client
 * has its own thread blocking in read(); we wake them via
 * shutdown(SHUT_RDWR) on the client fd (which causes the pending
 * read to return EOF) and then join. Order matters:
 *   1. running_ = false  (accept_loop will exit on next poll cycle)
 *   2. close(listen_fd_) (stops new connections; in-flight clients
 *      are unaffected)
 *   3. join accept_thread_ (no more clients spawn after this)
 *   4. shutdown each client fd to wake blocking read()s
 *   5. join each client thread
 *   6. close client fds (the threads' RAII guards do this too, but
 *      we run it again as a defensive measure on shutdown errors)
 *
 * @internal
 * @version 2.1.2
 */
void ExternalBridge::stop() {
    running_.store(false);
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // Wake every connected client's blocking read so the per-client
    // thread can observe the disconnect and exit. shutdown(SHUT_RDWR)
    // on a connected socket returns EOF to the peer; the read in
    // serve_client returns 0 and read_line returns "" → exits.
    std::vector<std::unique_ptr<ClientThread>> drained;
    {
        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        drained = std::move(client_threads_);
        client_threads_.clear();
    }
    for (auto& ct : drained) {
        if (ct->fd >= 0) { ::shutdown(ct->fd, SHUT_RDWR); }
    }
    for (auto& ct : drained) {
        if (ct->thread.joinable()) { ct->thread.join(); }
    }

    // Clean up socket file
    std::error_code ec;
    std::filesystem::remove(socket_path_, ec);
}

/**
 * @brief Reap finished client threads (called under client_threads_mutex_).
 *
 * Long-running bridges can see many connect/disconnect cycles. Without
 * reaping, the vector grows indefinitely with finished but
 * still-joinable thread handles. Each per-client thread sets its own
 * ``finished`` flag right before exit; the accept loop calls this
 * after every new connection to harvest exited entries.
 *
 * @internal
 * @version 2.1.2
 */
void ExternalBridge::reap_finished_clients_locked() {
    auto it = client_threads_.begin();
    while (it != client_threads_.end()) {
        if ((*it)->finished.load()) {
            if ((*it)->thread.joinable()) { (*it)->thread.join(); }
            it = client_threads_.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * @brief Background thread: accept connections and dispatch to per-client
 *        serve threads.
 *
 * Issue #4 (v2.1.2, part D): pre-2.1.2 this called ``serve_client(fd)``
 * inline on the accept thread. While one client was connected, the
 * loop blocked here and no further ``accept()`` happened — so a
 * single wedged client also blocked every other operator's recovery
 * attempt and made it impossible for two MCP clients (e.g. TUI +
 * Claude Code) to be connected simultaneously. Now each accepted
 * connection gets its own thread and the accept loop returns
 * immediately to ``poll`` for the next.
 *
 * Lifecycle: the per-client thread closes its fd before exit and
 * sets ``finished``; ``reap_finished_clients_locked()`` then joins
 * and erases. ``stop()`` covers the race where shutdown happens
 * mid-serve by ``shutdown(fd, SHUT_RDWR)`` to wake any blocking
 * read.
 *
 * @internal
 * @version 2.1.2
 */
void ExternalBridge::accept_loop() {
    while (running_.load()) {
        struct pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;

        int rc = poll(&pfd, 1, 500);  // 500ms timeout for shutdown check
        if (rc <= 0) { continue; }

        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) { continue; }

        logger->info("External MCP client connected (fd={})", client_fd);

        auto ct = std::make_unique<ClientThread>();
        ct->fd = client_fd;
        // Capture a raw pointer for the thread body so the unique_ptr
        // can stay in client_threads_ without aliasing.
        ClientThread* raw = ct.get();
        ct->thread = std::thread([this, raw]() {
            serve_client(raw->fd);
            ::close(raw->fd);
            raw->fd = -1;
            logger->info("External MCP client disconnected");
            raw->finished.store(true);
        });

        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        client_threads_.push_back(std::move(ct));
        reap_finished_clients_locked();
    }
}

/**
 * @brief Read one newline-delimited line from a socket fd.
 * @param fd Socket file descriptor.
 * @return Line content, or empty on error/disconnect.
 * @utility
 * @version 2.0.8
 */
static std::string read_line(int fd) {
    std::string line;
    char c;
    while (true) {
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) { return {}; }
        if (c == '\n') { return line; }
        line += c;
    }
}

/**
 * @brief Serve a single connected client until disconnect.
 *
 * Dispatches each line as a JSON-RPC message. Notifications
 * (no id) produce an empty response — nothing is written back.
 *
 * @param client_fd Connected socket file descriptor.
 * @internal
 * @version 2.0.6-rc16
 */
void ExternalBridge::serve_client(int client_fd) {
    subscribe(client_fd);
    struct Unsub {
        ExternalBridge* self;
        int fd;
        ~Unsub() { self->unsubscribe(fd); }
    } guard{this, client_fd};

    while (running_.load()) {
        auto line = read_line(client_fd);
        if (line.empty()) { break; }

        auto response = dispatch(line, client_fd);
        if (response.empty()) { continue; }  // notification — no reply
        response += '\n';

        ssize_t written = write(client_fd, response.c_str(),
                                response.size());
        if (written < 0) { break; }
    }
}

/**
 * @brief Build the MCP initialize response payload.
 * @return JSON result object.
 * @utility
 * @version 2.0.9
 */
static json initialize_result() {
    return {
        {"protocolVersion", "2025-06-18"},
        {"serverInfo", {{"name", "entropic"},
                        {"version", entropic_version()}}},
        {"capabilities", {{"tools", json::object()}}}
    };
}

/**
 * @brief Dispatch a JSON-RPC request and return the response.
 *
 * Per JSON-RPC 2.0 spec, messages without an "id" field are
 * notifications and MUST NOT receive a response. Returns empty
 * string for notifications so the caller knows not to write back.
 *
 * @param request Raw JSON-RPC request string.
 * @param client_fd Socket fd for streaming progress (entropic.ask).
 * @return JSON-RPC response string, or empty for notifications.
 * @internal
 * @version 2.0.11
 */
std::string ExternalBridge::dispatch(
    const std::string& request, int client_fd) {
    auto req = json::parse(request, nullptr, false);
    // Parse error or notification (no id) → no dispatch
    if (req.is_discarded() || !req.contains("id")) {
        return req.is_discarded()
            ? rpc_err(nullptr, -32700, "Parse error")
            : std::string{};
    }

    json id = req["id"];
    std::string method = req.value("method", std::string{});
    json params = req.value("params", json::object());
    json result;

    if      (method == "initialize")  { result = initialize_result(); }
    else if (method == "tools/list")  { result = {{"tools", tool_definitions()}}; }
    else if (method == "tools/call")  {
        auto id_str = id.is_string() ? id.get<std::string>()
                                     : id.dump();
        result = dispatch_tool(handle_, this, params, client_fd, id_str);
    }
    else if (method == "shutdown" || method == "exit") { result = json::object(); }
    else { return rpc_err(id, -32601, "Unknown method: " + method); }
    return rpc_ok(id, result);
}

// ── Async task support ───────────────────────────────────

/**
 * @brief State observer that projects VERIFYING onto task phase.
 * @internal
 * @version 2.1.0
 */
static void phase_observer_cb(int state, void* ud) {
    auto* self = static_cast<ExternalBridge*>(ud);
    if (state != ENTROPIC_AGENT_STATE_VERIFYING) { return; }
    std::lock_guard<std::mutex> lock(self->tasks_mutex_);
    // E5+E6 (2.1.0): discard stale callbacks fired after detach_phase_observer
    // incremented observer_gen_. attached_gen_ was captured at attach time.
    if (self->observer_call_is_stale()) { return; }
    auto it = self->tasks_for_cancel().find(self->active_task_id_for_observer());
    if (it == self->tasks_for_cancel().end()) { return; }
    // First VERIFYING = "validating"; subsequent VERIFYING transitions
    // on the same task indicate revision retries → "revising".
    it->second.phase = (it->second.phase == "validating"
                         || it->second.phase == "revising")
        ? "revising" : "validating";
}

/**
 * @brief Install phase observer scoped to one task.
 *
 * Increments observer_gen_ under tasks_mutex_ and captures it as
 * attached_gen_ so phase_observer_cb can detect stale post-detach
 * fires via a simple generation comparison. (E5+E6, 2.1.0)
 *
 * @param task_id Task whose phase transitions the observer tracks.
 * @internal
 * @version 2.1.0
 */
void ExternalBridge::attach_phase_observer(const std::string& task_id) {
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        active_task_id_ = task_id;
        attached_gen_ = ++observer_gen_;
    }
    entropic_set_state_observer(handle_, phase_observer_cb, this);
}

/**
 * @brief Clear phase observer and the active-task pointer.
 *
 * Increments observer_gen_ under the lock before clearing
 * active_task_id_ — any in-flight phase_observer_cb will see a
 * generation mismatch and return immediately without accessing
 * stale state. entropic_set_state_observer(nullptr) is called after
 * the lock is released. (E5+E6, 2.1.0)
 *
 * @internal
 * @version 2.1.0
 */
void ExternalBridge::detach_phase_observer() {
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        ++observer_gen_;
        active_task_id_.clear();
    }
    entropic_set_state_observer(handle_, nullptr, nullptr);
}

/**
 * @brief Run an async entropic.ask in a detached background thread.
 *
 * Creates a task registry entry, runs the engine, stores the result,
 * and pushes a notifications/ask_complete to the active client fd.
 *
 * @param prompt User prompt.
 * @param task_id Assigned task ID.
 * @param client_fd Socket fd for completion notification.
 * @internal
 * @version 2.1.2
 */
void ExternalBridge::run_async_ask(
    const std::string& prompt,
    const std::string& task_id,
    int client_fd) {
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        AsyncTask t;
        t.status = "queued";
        t.phase = "queued";
        t.created = std::chrono::steady_clock::now();
        tasks_[task_id] = std::move(t);
    }

    std::thread([this, prompt, task_id, client_fd]() {
        update_task_phase(task_id, "running", "running");
        attach_phase_observer(task_id);

        char* result_json = nullptr;
        auto err = entropic_run(handle_, prompt.c_str(), &result_json);

        detach_phase_observer();

        auto final_state = derive_async_final_state(
            handle_, err, result_json);

        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            auto it = tasks_.find(task_id);
            if (it != tasks_.end()) {
                it->second.status = final_state.status;
                it->second.phase = final_state.phase;
                it->second.result = final_state.text;
            }
        }
        auto status = final_state.status;

        // Issue #4 (v2.1.2, parts A+B): emit the spec-defined
        // ``notifications/progress`` method instead of the previous
        // non-spec ``notifications/ask_complete``. MCP-compliant
        // clients are obligated to drain ``notifications/progress``
        // (it's in the documented set); some clients silently
        // buffered or stalled on the unknown method name, which
        // combined with the bridge's blocking broadcast (fixed in
        // part C of this release) produced the deadlock observed
        // against entropic-explorer ↔ Claude Code in the field.
        //
        // ``progressToken`` is the ``task_id`` so the consumer can
        // correlate the notification back to the originating
        // ``entropic.ask`` response (which carried the same
        // ``task_id``). The result body is NO LONGER shipped inline
        // — consumers fetch via ``entropic.ask_status``. This caps
        // notification size at ~200 bytes regardless of generated
        // output, eliminating a real DoS surface (a 50KB result
        // would otherwise flood the broadcast write path on every
        // subscriber). ``status`` rides in ``message`` so consumers
        // can branch on done / error / cancelled without an extra
        // round-trip just to learn which result kind to fetch.
        json notif = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/progress"},
            {"params", {
                {"progressToken", task_id},
                {"progress", 100},
                {"total", 100},
                {"message", status}
            }}
        };
        broadcast_notification(notif);

        cleanup_expired_tasks();
        logger->info("Async task {} completed: {}", task_id, status);
    }).detach();
}

/**
 * @brief Remove tasks older than 15 minutes from the registry.
 * @internal
 * @version 2.0.11
 */
/**
 * @brief Add a connected fd to the subscriber set.
 * @param fd Client socket fd.
 * @internal
 * @version 2.0.6-rc16
 */
void ExternalBridge::subscribe(int fd) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.insert(fd);
}

/**
 * @brief Remove an fd from the subscriber set.
 * @param fd Client socket fd being closed.
 * @internal
 * @version 2.0.6-rc16
 */
void ExternalBridge::unsubscribe(int fd) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.erase(fd);
}

/**
 * @brief Broadcast a JSON-RPC notification to every subscriber.
 *
 * Snapshots the subscriber set under the lock, releases the lock before
 * writing, then re-acquires to remove dead fds. This prevents one
 * blocked or slow client from stalling all other broadcasts.
 *
 * Issue #4 (v2.1.2, part C): write is non-blocking via
 * ``send(MSG_DONTWAIT)``. A subscriber whose recv buffer is full
 * (slow / non-draining consumer) returns ``EAGAIN``/``EWOULDBLOCK``
 * and is dropped on the same path as ``EBADF``/``EPIPE``. Pre-2.1.2
 * the broadcast used blocking ``write()``, which let one stalled
 * consumer wedge the async-task thread that emitted the notification
 * (the field-observed deadlock against entropic-explorer ↔ Claude
 * Code; see issue #4 reproduction).
 *
 * A partial write is also treated as a drop. The dropped subscriber
 * loses the in-flight notification — acceptable because (a) post-#4
 * notifications are tiny progress signals (consumers fetch state via
 * ``ask_status``, not from the notification body), and (b) a
 * subscriber that can't accept ~200 bytes of buffered notification is
 * unhealthy anyway. The longer-term replacement is a per-subscriber
 * outbound queue + writer thread (see proposal
 * ``.claude/proposals/BACKLOG/P2-20260429-001-async-bridge-io-architecture.md``).
 *
 * Subscribers added between snapshot and dead-fd cleanup miss this
 * broadcast — correct, as they subscribed after the message was
 * initiated.
 *
 * @param notif JSON-RPC notification object.
 * @internal
 * @version 2.1.2
 */
void ExternalBridge::broadcast_notification(const json& notif) {
    auto payload = notif.dump() + "\n";

    std::vector<int> snapshot;
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        snapshot.assign(subscribers_.begin(), subscribers_.end());
    }

    std::vector<int> dead;
    for (int fd : snapshot) {
        ssize_t rc = ::send(fd, payload.c_str(), payload.size(),
                            MSG_DONTWAIT | MSG_NOSIGNAL);
        if (rc < 0) {
            // EAGAIN / EWOULDBLOCK: peer recv buffer full (slow consumer).
            // EBADF / EPIPE / ECONNRESET: peer closed or otherwise dead.
            // All collapse to "drop" — the long-term per-subscriber
            // queue lives in proposal P2-20260429-001.
            logger->warn("Subscriber fd {} send failed (errno={}) — dropping",
                         fd, errno);
            dead.push_back(fd);
        } else if (static_cast<size_t>(rc) < payload.size()) {
            logger->warn("Subscriber fd {} partial send ({}/{}) — dropping",
                         fd, rc, payload.size());
            dead.push_back(fd);
        }
    }

    if (!dead.empty()) {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        for (int fd : dead) { subscribers_.erase(fd); }
    }
}

/**
 * @brief Update the status/phase of a tracked task.
 * @param task_id Task identifier.
 * @param status New coarse status string.
 * @param phase New granular phase string.
 * @internal
 * @version 2.0.6-rc16
 */
void ExternalBridge::update_task_phase(const std::string& task_id,
                                       const std::string& status,
                                       const std::string& phase) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) { return; }
    it->second.status = status;
    it->second.phase = phase;
}

/**
 * @brief Remove tasks older than 15 minutes from the registry.
 * @internal
 * @version 2.0.11
 */
void ExternalBridge::cleanup_expired_tasks() {
    auto cutoff = std::chrono::steady_clock::now()
        - std::chrono::minutes(15);
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    for (auto it = tasks_.begin(); it != tasks_.end(); ) {
        if (it->second.created < cutoff) {
            it = tasks_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace entropic
