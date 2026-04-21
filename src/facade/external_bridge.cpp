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

#include <cstring>
#include <functional>
#include <sstream>
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
 * @version 2.0.8
 */
static json tool_definitions() {
    return json::array({
        {{"name", "entropic.ask"},
         {"description",
          "Submit a prompt to the running entropic engine. Returns "
          "the full response. Conversation context is retained."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {{"prompt", {
                {"type", "string"},
                {"description", "User message"}
            }}}},
            {"required", json::array({"prompt"})}
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
 * @brief Handle entropic.ask — run a prompt through the engine.
 * @param handle Engine handle.
 * @param args Tool arguments.
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.8
 */
static json handle_ask(entropic_handle_t handle, const json& args) {
    auto it = args.find("prompt");
    if (it == args.end() || !it->is_string()) {
        return tool_text("error: missing 'prompt' argument");
    }
    std::string prompt = it->get<std::string>();
    std::string accumulator;
    auto on_token = [](const char* tok, size_t len, void* ud) {
        static_cast<std::string*>(ud)->append(tok, len);
    };
    auto err = entropic_run_streaming(
        handle, prompt.c_str(), on_token, &accumulator, nullptr);
    if (err != ENTROPIC_OK) {
        const char* msg = entropic_last_error(handle);
        return tool_text(std::string("error: ") + (msg ? msg : "unknown"));
    }
    return tool_text(accumulator);
}

/**
 * @brief Handle entropic.status.
 * @param handle Engine handle.
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.8
 */
static json handle_status(entropic_handle_t handle) {
    size_t count = 0;
    entropic_context_count(handle, &count);
    std::ostringstream os;
    os << "entropic " << entropic_version()
       << "\nmessages: " << count;
    return tool_text(os.str());
}

/**
 * @brief Handle entropic.context_clear.
 * @param handle Engine handle.
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.8
 */
static json handle_clear(entropic_handle_t handle) {
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

// ── Dispatch ─────────────────────────────────────────────

/**
 * @brief Dispatch a tools/call to the appropriate handler.
 * @param handle Engine handle.
 * @param params JSON-RPC params (name + arguments).
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.8
 */
static json dispatch_tool(entropic_handle_t handle,
                          const json& params) {
    std::string name = params.value("name", std::string{});
    json args = params.value("arguments", json::object());
    json result;
    if      (name == "entropic.ask")           { result = handle_ask(handle, args); }
    else if (name == "entropic.status")        { result = handle_status(handle); }
    else if (name == "entropic.context_clear") { result = handle_clear(handle); }
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
 * @brief Stop the accept loop and close the socket.
 * @internal
 * @version 2.0.8
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
    // Clean up socket file
    std::error_code ec;
    std::filesystem::remove(socket_path_, ec);
}

/**
 * @brief Background thread: accept connections and serve.
 * @internal
 * @version 2.0.8
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

        logger->info("External MCP client connected");
        serve_client(client_fd);
        ::close(client_fd);
        logger->info("External MCP client disconnected");
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
 * @param client_fd Connected socket file descriptor.
 * @internal
 * @version 2.0.8
 */
void ExternalBridge::serve_client(int client_fd) {
    while (running_.load()) {
        auto line = read_line(client_fd);
        if (line.empty()) { break; }

        auto response = dispatch(line);
        response += '\n';

        ssize_t written = write(client_fd, response.c_str(),
                                response.size());
        if (written < 0) { break; }
    }
}

/**
 * @brief Dispatch a JSON-RPC request and return the response.
 * @param request Raw JSON-RPC request string.
 * @return JSON-RPC response string.
 * @internal
 * @version 2.0.8
 */
/**
 * @brief Build the MCP initialize response payload.
 * @return JSON result object.
 * @utility
 * @version 2.0.8
 */
static json initialize_result() {
    return {
        {"protocolVersion", "2024-11-05"},
        {"serverInfo", {{"name", "entropic"},
                        {"version", entropic_version()}}},
        {"capabilities", {{"tools", json::object()}}}
    };
}

/**
 * @brief Dispatch a JSON-RPC request and return the response.
 * @param request Raw JSON-RPC request string.
 * @return JSON-RPC response string.
 * @internal
 * @version 2.0.8
 */
std::string ExternalBridge::dispatch(const std::string& request) {
    auto req = json::parse(request, nullptr, false);
    if (req.is_discarded()) {
        return rpc_err(nullptr, -32700, "Parse error");
    }

    json id = req.value("id", json(nullptr));
    std::string method = req.value("method", std::string{});
    json params = req.value("params", json::object());
    json result;

    if      (method == "initialize")  { result = initialize_result(); }
    else if (method == "tools/list")  { result = {{"tools", tool_definitions()}}; }
    else if (method == "tools/call")  { result = dispatch_tool(handle_, params); }
    else if (method == "shutdown" || method == "exit") { result = json::object(); }
    else { return rpc_err(id, -32601, "Unknown method: " + method); }
    return rpc_ok(id, result);
}

} // namespace entropic
