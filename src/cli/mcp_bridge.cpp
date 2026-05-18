// SPDX-License-Identifier: Apache-2.0
/**
 * @file mcp_bridge.cpp
 * @brief `entropic mcp-bridge` — stdio↔unix-socket relay for MCP.
 *
 * Pure protocol adapter. Connects to a running engine's external MCP
 * unix socket and shuttles JSON-RPC 2.0 bytes between the local stdio
 * pair (read from an MCP client like Claude Code) and the socket pair
 * (read by the engine's ExternalBridge). The bridge:
 *
 *   - Never creates an engine handle. Never loads a model. Owns no
 *     state beyond two file descriptors.
 *   - Discovers the socket path by hashing the canonicalized project
 *     directory (same `compute_socket_path` used by the engine), so
 *     `.mcp.json` consumers do not have to know the path.
 *   - Fails fast with a diagnostic naming cwd, canonical path, hash,
 *     and computed socket path when no socket exists.
 *
 * The relay is byte-transparent: it does not parse JSON-RPC at all,
 * so server-initiated progress notifications, streaming, and
 * out-of-order responses all pass through unmodified. Both sides use
 * newline-framed messages (the wire convention established by the
 * engine's ExternalBridge).
 *
 * Usage in .mcp.json:
 * @code
 *   {"mcpServers": {"entropic": {
 *     "type": "stdio",
 *     "command": "entropic",
 *     "args": ["mcp-bridge"]
 *   }}}
 * @endcode
 *
 * An engine must already be running for the same project directory
 * (TUI, consumer app, or a future headless server). The bridge is an
 * *optional service* on top of that engine — never a substitute for
 * it.
 *
 * @par MCP transport compliance
 * - **Stdio framing**: MCP stdio transport is newline-delimited
 *   JSON-RPC 2.0 (one message per line, UTF-8, no embedded newlines).
 *   The relay is byte-transparent and preserves framing because both
 *   the client (e.g. Claude Code) and the engine's ExternalBridge
 *   emit complete `\n`-terminated frames; the relay merely shuttles
 *   bytes in order.
 * - **Stdout discipline**: stdout is reserved for relayed JSON-RPC
 *   bytes only. All diagnostics go to stderr (per the MCP spec's
 *   stdio transport contract). The bridge writes nothing to stdout
 *   itself — no banners, no log lines, no flush noise.
 * - **Bidirectional traffic**: a `poll(2)` loop handles both
 *   directions independently, so server-initiated frames
 *   (`notifications/progress`, streaming partial results, etc.) reach
 *   the client even when no request is in flight. The pre-2.1.7
 *   `mcp-connect` did synchronous request→response and would have
 *   stalled server-pushed frames.
 * - **`initialize` / `notifications/initialized` handshake**: the
 *   bridge is transparent — the handshake occurs end-to-end between
 *   the MCP client and the engine, exactly as the spec describes.
 * - **`shutdown` / `exit`**: handled by the engine's dispatch. On
 *   stdin EOF (client went away) the bridge half-closes the socket
 *   write side via `shutdown(SHUT_WR)`, which the engine's
 *   `serve_client` observes as an empty read and uses to tear down
 *   the per-client thread cleanly.
 * - **No protocol coupling**: the bridge does not parse JSON-RPC at
 *   all, so future MCP protocol revisions ride through without
 *   needing changes here. The contract sits with the engine's
 *   `ExternalBridge::dispatch`.
 *
 * @version 2.1.7
 */

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace entropic {
// Forward-declared from include/entropic/mcp/mcp_json_discovery.h; pulled in
// via libentropic-mcp at link time so the CLI does not have to include the
// mcp internal header (which transitively pulls nlohmann/json_fwd).
std::filesystem::path compute_socket_path(
    const std::filesystem::path& project_dir);
}  // namespace entropic

namespace entropic::cli {

namespace {

constexpr size_t kRelayBufSize = 8192;

/**
 * @brief Parse a --flag VALUE pair from argv.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param flag Flag name (e.g. "--project-dir").
 * @return Provided value or empty if absent.
 * @utility
 * @version 2.1.7
 */
std::string parse_flag(int argc, char* argv[], const char* flag)
{
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return argv[i + 1];
        }
    }
    return {};
}

/**
 * @brief Connect to a unix domain socket.
 * @param path Socket path.
 * @return Connected fd, or -1 on failure (errno set).
 * @utility
 * @version 2.1.7
 */
int connect_unix_socket(const std::string& path)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    bool ok = fd >= 0 && path.size() < sizeof(sockaddr_un::sun_path);
    int saved = ok ? 0 : (fd < 0 ? errno : ENAMETOOLONG);
    if (ok) {
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(),
                     sizeof(addr.sun_path) - 1);
        ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                       sizeof(addr)) == 0;
        if (!ok) { saved = errno; }
    }
    if (!ok) {
        if (fd >= 0) { ::close(fd); }
        errno = saved;
        fd = -1;
    }
    return fd;
}

/**
 * @brief Emit the no-engine diagnostic and return exit code 1.
 *
 * Names every component that contributes to socket discovery so a user
 * can pinpoint where the bridge and engine disagree. This is the
 * primary user-facing failure mode and must be diagnosable without
 * running strace.
 *
 * @param requested  Project dir as provided (cwd if --project-dir omitted).
 * @param canonical  weakly_canonical(requested).
 * @param socket     Computed socket path.
 * @param why        errno-derived reason text.
 * @return 1 (process exit code).
 * @utility
 * @version 2.1.7
 */
int emit_no_engine_error(
    const std::filesystem::path& requested,
    const std::filesystem::path& canonical,
    const std::filesystem::path& socket,
    const char* why)
{
    std::fprintf(stderr,
        "entropic mcp-bridge: no running engine.\n"
        "  project_dir (requested):  %s\n"
        "  project_dir (canonical):  %s\n"
        "  expected socket:          %s\n"
        "  connect failed:           %s\n"
        "\n"
        "An engine must be running for this project. Start one via the\n"
        "engine host that owns the model (TUI, consumer app, or headless\n"
        "server). mcp-bridge is a relay only — it does not load the\n"
        "model itself.\n",
        requested.c_str(), canonical.c_str(),
        socket.c_str(), why ? why : "unknown");
    return 1;
}

/**
 * @brief Forward bytes from src_fd to dst_fd.
 * @param src_fd Read source.
 * @param dst_fd Write destination.
 * @return true if data forwarded, false on EOF/error.
 * @utility
 * @version 2.1.7
 */
bool pump_once(int src_fd, int dst_fd)
{
    char buf[kRelayBufSize];
    ssize_t n = ::read(src_fd, buf, sizeof(buf));
    if (n <= 0) { return false; }
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = ::write(dst_fd, buf + off, n - off);
        if (w <= 0) { return false; }
        off += w;
    }
    return true;
}

/**
 * @brief Service a single ready pollfd revent set.
 * @utility
 * @version 2.1.7
 */
void service_revents(struct pollfd* fds, int sock_fd,
                     bool& stdin_open, bool& sock_open)
{
    if (stdin_open && (fds[0].revents & (POLLIN | POLLHUP))) {
        if (!pump_once(STDIN_FILENO, sock_fd)) {
            stdin_open = false;
            ::shutdown(sock_fd, SHUT_WR);
        }
    }
    if (sock_open && (fds[1].revents & (POLLIN | POLLHUP))) {
        if (!pump_once(sock_fd, STDOUT_FILENO)) {
            sock_open = false;
        }
    }
}

/**
 * @brief Bidirectional byte relay between stdio and a connected socket.
 *
 * Pure poll loop. No JSON parsing, no framing knowledge. Either side
 * may send data at any time; server-initiated progress notifications
 * and streaming responses pass through unmodified.
 *
 * Exits cleanly on EOF from either side. Half-close on stdin (client
 * went away) propagates by closing the socket write side; the engine's
 * ExternalBridge observes that and tears down its per-client thread.
 *
 * @param sock_fd Connected unix socket fd.
 * @internal
 * @version 2.1.7
 */
void relay_loop(int sock_fd)
{
    struct pollfd fds[2] = {};
    fds[0].fd = STDIN_FILENO;
    fds[1].fd = sock_fd;

    bool stdin_open = true;
    bool sock_open = true;
    while (stdin_open || sock_open) {
        fds[0].events = stdin_open ? POLLIN : 0;
        fds[1].events = sock_open ? POLLIN : 0;
        int rc = ::poll(fds, 2, -1);
        if (rc < 0 && errno != EINTR) { break; }
        if (rc > 0) {
            service_revents(fds, sock_fd, stdin_open, sock_open);
        }
    }
}

}  // namespace

/**
 * @brief Entry point for the `mcp-bridge` subcommand.
 *
 * Resolves the project dir, computes the engine's unix socket path
 * via the same deterministic hash the engine uses, connects, then
 * relays bytes between stdio and the socket until either side closes.
 *
 * @param argc Argument count (after the "mcp-bridge" subcommand).
 * @param argv Argument vector. argv[0] is "mcp-bridge".
 * @return 0 on clean exit, 1 if no engine is reachable.
 * @internal
 * @version 2.1.7
 */
int run_mcp_bridge(int argc, char* argv[])
{
    // --socket PATH overrides discovery. Primary uses: deterministic
    // tests, and users whose engine is configured with a non-default
    // socket path. Absent the override, the bridge derives the path
    // from the canonicalized project_dir hash — same scheme as the
    // engine's ExternalBridge.
    std::string explicit_socket = parse_flag(argc, argv, "--socket");
    std::string requested = parse_flag(argc, argv, "--project-dir");
    std::filesystem::path requested_path = requested.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(requested);

    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(requested_path, ec);
    if (ec) { canonical = requested_path; }

    std::filesystem::path socket_path = explicit_socket.empty()
        ? entropic::compute_socket_path(canonical)
        : std::filesystem::path(explicit_socket);

    int fd = connect_unix_socket(socket_path.string());
    if (fd < 0) {
        return emit_no_engine_error(
            requested_path, canonical, socket_path,
            std::strerror(errno));
    }

    relay_loop(fd);
    ::close(fd);
    return 0;
}

}  // namespace entropic::cli
