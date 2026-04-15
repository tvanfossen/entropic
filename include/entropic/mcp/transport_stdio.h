// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file transport_stdio.h
 * @brief Stdio transport for external MCP servers.
 *
 * Spawns a child process via posix_spawn(), communicates over
 * stdin/stdout pipes with newline-delimited JSON-RPC messages.
 * Stderr is captured and forwarded to spdlog at WARNING level.
 *
 * @version 1.8.7
 */

#pragma once

#include <entropic/mcp/transport.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

namespace entropic {

/**
 * @brief Stdio transport: fork/exec child, pipe stdin/stdout.
 *
 * JSON-RPC 2.0 over newline-delimited JSON. Child process lifetime
 * tied to transport lifetime (SIGTERM on close, SIGKILL if needed).
 *
 * @version 1.8.7
 */
class StdioTransport : public Transport {
public:
    /**
     * @brief Construct with command, arguments, and environment.
     * @param command Executable to spawn.
     * @param args Command-line arguments.
     * @param env Environment variable overrides (merged with parent).
     * @param default_timeout_ms Default timeout for send_request (0 = 30s).
     * @version 1.8.7
     */
    StdioTransport(
        std::string command,
        std::vector<std::string> args,
        std::map<std::string, std::string> env = {},
        uint32_t default_timeout_ms = 30000);

    ~StdioTransport() override;

    /**
     * @brief Spawn child process and open pipes.
     * @return true on success.
     * @version 1.8.7
     */
    bool open() override;

    /**
     * @brief Send SIGTERM, reap child, close pipes.
     * @version 1.8.7
     */
    void close() override;

    /**
     * @brief Send JSON-RPC request via stdin, read response from stdout.
     * @param request_json JSON-RPC request string.
     * @param timeout_ms Timeout in milliseconds (0 = default).
     * @return JSON-RPC response string, or empty on error/timeout.
     * @version 1.8.7
     */
    std::string send_request(
        const std::string& request_json,
        uint32_t timeout_ms = 0) override;

    /**
     * @brief Check if child process is alive.
     * @return true if connected.
     * @version 1.8.7
     */
    bool is_connected() const override;

private:
    std::string command_;                        ///< Executable path
    std::vector<std::string> args_;              ///< Command-line arguments
    std::map<std::string, std::string> env_;     ///< Environment overrides
    uint32_t default_timeout_ms_;                ///< Default request timeout

    pid_t child_pid_{-1};                        ///< Child process PID
    int stdin_fd_{-1};                           ///< Write end of child stdin pipe
    int stdout_fd_{-1};                          ///< Read end of child stdout pipe
    int stderr_fd_{-1};                          ///< Read end of child stderr pipe
    std::atomic<bool> connected_{false};         ///< Connection state
    std::thread stderr_thread_;                  ///< Stderr forwarding thread
    std::mutex io_mutex_;                        ///< Guards pipe I/O

    /**
     * @brief Build merged environment for child process.
     * @return Vector of "KEY=VALUE" strings.
     * @utility
     * @version 1.8.7
     */
    std::vector<std::string> build_env() const;

    /**
     * @brief Spawn child process using posix_spawn.
     * @param stdin_r Child stdin read end.
     * @param stdout_w Child stdout write end.
     * @param stderr_w Child stderr write end.
     * @param env_strs Merged environment strings.
     * @return true on success, child_pid_ set.
     * @utility
     * @version 1.8.7
     */
    bool spawn_child(int stdin_r, int stdout_w, int stderr_w,
                     const std::vector<std::string>& env_strs);

    /**
     * @brief Create pipe pair and set close-on-exec.
     * @param[out] read_fd Read end.
     * @param[out] write_fd Write end.
     * @return true on success.
     * @utility
     * @version 1.8.7
     */
    static bool create_pipe(int& read_fd, int& write_fd);

    /**
     * @brief Read a single newline-delimited line from fd.
     * @param fd File descriptor to read from.
     * @param timeout_ms Timeout in milliseconds.
     * @return Line content (without newline), or empty on error/timeout.
     * @utility
     * @version 1.8.8
     */
    static std::string read_line(int fd, uint32_t timeout_ms);

    /**
     * @brief Poll fd for readability within remaining deadline.
     * @param fd File descriptor.
     * @param deadline Absolute deadline.
     * @return 1 if ready, 0 on timeout, -1 on error.
     * @utility
     * @version 1.8.8
     */
    static int poll_until_ready(
        int fd,
        std::chrono::steady_clock::time_point deadline);

    /**
     * @brief Create all three pipe pairs for child communication.
     * @param[out] fds Array of 6 fds.
     * @return true if all pipes created successfully.
     * @utility
     * @version 1.8.7
     */
    bool create_all_pipes(int (&fds)[6]);

    /**
     * @brief Spawn child process with pipes (open helper).
     * @return true on success (stdin_fd_, stdout_fd_, stderr_fd_ set).
     * @utility
     * @version 1.8.7
     */
    bool open_child_process();

    /**
     * @brief Forward stderr lines to spdlog WARNING.
     * @callback
     * @version 1.8.7
     */
    void stderr_reader_loop();

    /**
     * @brief Terminate and reap child process.
     * @utility
     * @version 1.8.7
     */
    void terminate_child();

    /**
     * @brief Close a file descriptor if open, set to -1.
     * @param fd File descriptor reference.
     * @utility
     * @version 1.8.7
     */
    static void close_fd(int& fd);
};

} // namespace entropic
