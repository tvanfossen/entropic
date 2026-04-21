// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file mcp_connect.cpp
 * @brief `entropic mcp-connect` — stdio-to-unix-socket relay for MCP.
 *
 * Connects to a running engine's external MCP bridge (unix socket)
 * and relays JSON-RPC messages between stdin/stdout and the socket.
 * This allows Claude Code and other stdio-based MCP clients to talk
 * to an engine that's already running inside a consumer app.
 *
 * JSON-RPC 2.0 notifications (messages without an "id" field) are
 * forwarded to the bridge but no response is expected — the relay
 * does not block waiting for a reply.
 *
 * Usage in .mcp.json:
 *   {"mcpServers": {"entropic-explorer": {
 *     "type": "stdio",
 *     "command": "entropic",
 *     "args": ["mcp-connect", "--socket", "/tmp/entropic-explorer.sock"]
 *   }}}
 *
 * @version 2.0.10
 */

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace entropic::cli {

/**
 * @brief Parse --socket argument from argv.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Socket path, or empty if not found.
 * @utility
 * @version 2.0.8
 */
static std::string parse_socket_arg(int argc, char* argv[]) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0) {
            return argv[i + 1];
        }
    }
    return {};
}

/**
 * @brief Connect to a unix domain socket.
 * @param path Socket path.
 * @return Connected fd, or -1 on failure.
 * @utility
 * @version 2.0.8
 */
static int connect_socket(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { return -1; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr),
                sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/**
 * @brief Read one newline-delimited line from a socket fd.
 * @param fd File descriptor.
 * @return Line content, or empty on EOF/error.
 * @utility
 * @version 2.0.8
 */
static std::string read_line_fd(int fd) {
    std::string line;
    char c;
    while (true) {
        ssize_t n = ::read(fd, &c, 1);
        if (n <= 0) { return {}; }
        if (c == '\n') { return line; }
        line += c;
    }
}

/**
 * @brief Check if a JSON-RPC message is a notification (no "id" field).
 * @param line Raw JSON string.
 * @return true if the message has no "id" field.
 * @utility
 * @version 2.0.10
 */
static bool is_notification(const std::string& line) {
    auto j = nlohmann::json::parse(line, nullptr, false);
    return !j.is_discarded() && !j.contains("id");
}

/**
 * @brief Run the mcp-connect stdio-to-socket relay.
 *
 * Reads JSON-RPC messages from stdin, forwards to the unix socket.
 * For requests (messages with "id"), waits for a response and writes
 * it to stdout. Notifications (no "id") are forwarded but no response
 * is expected — the relay continues immediately.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on clean exit, 1 on error.
 * @internal
 * @version 2.0.10
 */
int run_mcp_connect(int argc, char* argv[]) {
    auto socket_path = parse_socket_arg(argc, argv);
    if (socket_path.empty()) {
        std::fprintf(stderr,
            "entropic mcp-connect: --socket PATH required\n");
        return 1;
    }

    int fd = connect_socket(socket_path);
    if (fd < 0) {
        std::fprintf(stderr,
            "entropic mcp-connect: cannot connect to %s: %s\n",
            socket_path.c_str(), std::strerror(errno));
        return 1;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) { continue; }
        bool notify = is_notification(line);
        line += '\n';
        if (::write(fd, line.c_str(), line.size()) < 0) { break; }
        if (notify) { continue; }  // no response expected
        auto response = read_line_fd(fd);
        if (response.empty()) { break; }
        std::cout << response << '\n';
        std::cout.flush();
    }

    ::close(fd);
    return 0;
}

} // namespace entropic::cli
