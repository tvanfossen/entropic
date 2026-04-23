// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file transport_stdio.cpp
 * @brief StdioTransport implementation.
 * @version 1.8.7
 */

#include <entropic/mcp/transport_stdio.h>
#include <entropic/types/logging.h>

#include <cerrno>
#include <cstring>
#include <chrono>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
/**
 * @brief Ignore SIGPIPE globally (write to dead pipe returns EPIPE).
 * @utility
 * @version 1.8.7
 */
struct IgnoreSigpipe {
    /**
     * @brief Install SIGPIPE handler at static init.
     * @utility
     * @version 1.8.7
     */
    IgnoreSigpipe() { signal(SIGPIPE, SIG_IGN); }
};
static IgnoreSigpipe ignore_sigpipe;
} // namespace

extern char** environ;

static auto logger = entropic::log::get("mcp.transport.stdio");

namespace entropic {

/**
 * @brief Construct with command, arguments, and environment.
 * @param command Executable to spawn.
 * @param args Command-line arguments.
 * @param env Environment variable overrides.
 * @param default_timeout_ms Default request timeout.
 * @internal
 * @version 1.8.7
 */
StdioTransport::StdioTransport(
    std::string command,
    std::vector<std::string> args,
    std::map<std::string, std::string> env,
    uint32_t default_timeout_ms)
    : command_(std::move(command)),
      args_(std::move(args)),
      env_(std::move(env)),
      default_timeout_ms_(default_timeout_ms) {}

/**
 * @brief Destructor — ensures child is cleaned up.
 * @internal
 * @version 1.8.7
 */
StdioTransport::~StdioTransport() {
    close();
}

/**
 * @brief Create all three pipe pairs for child communication.
 * @param[out] fds Array of 6 fds: stdin_r, stdin_w, stdout_r, stdout_w, stderr_r, stderr_w.
 * @return true if all pipes created successfully.
 * @utility
 * @version 1.8.8
 */
bool StdioTransport::create_all_pipes(int (&fds)[6]) {
    bool ok = create_pipe(fds[0], fds[1]) &&
              create_pipe(fds[2], fds[3]) &&
              create_pipe(fds[4], fds[5]);
    if (!ok) {
        logger->error("Failed to create pipes: {}", strerror(errno));
        for (auto& fd : fds) { close_fd(fd); }
    }
    return ok;
}

/**
 * @brief Spawn child process and open pipes.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool StdioTransport::open() {
    if (connected_) {
        return true;
    }

    if (!open_child_process()) {
        return false;
    }

    connected_ = true;
    stderr_thread_ = std::thread(
        &StdioTransport::stderr_reader_loop, this);

    logger->info("Spawned child process PID {} for '{}'",
                 child_pid_, command_);
    return true;
}

/**
 * @brief Spawn child process with pipes (open helper).
 * @return true on success (stdin_fd_, stdout_fd_, stderr_fd_ set).
 * @utility
 * @version 1.8.7
 */
bool StdioTransport::open_child_process() {
    int fds[6] = {-1, -1, -1, -1, -1, -1};
    if (!create_all_pipes(fds)) {
        return false;
    }

    auto env_strs = build_env();
    bool ok = spawn_child(fds[0], fds[3], fds[5], env_strs);

    // Close child-side pipe ends in parent
    close_fd(fds[0]);
    close_fd(fds[3]);
    close_fd(fds[5]);

    if (!ok) {
        close_fd(fds[1]);
        close_fd(fds[2]);
        close_fd(fds[4]);
        return false;
    }

    stdin_fd_ = fds[1];
    stdout_fd_ = fds[2];
    stderr_fd_ = fds[4];
    return true;
}

/**
 * @brief Send SIGTERM, reap child, close pipes.
 * @internal
 * @version 1.8.7
 */
void StdioTransport::close() {
    connected_ = false;

    terminate_child();
    close_fd(stdin_fd_);
    close_fd(stdout_fd_);
    close_fd(stderr_fd_);

    if (stderr_thread_.joinable()) {
        stderr_thread_.join();
    }

    logger->info("Closed stdio transport for '{}'", command_);
}

/**
 * @brief Send JSON-RPC request via stdin, read response from stdout.
 * @param request_json JSON-RPC request string.
 * @param timeout_ms Timeout (0 = default).
 * @return Response string, or empty on error.
 * @internal
 * @version 2.0.6-rc16
 */
std::string StdioTransport::send_request(
    const std::string& request_json,
    uint32_t timeout_ms) {

    if (!connected_ || cancel_flag_.load(std::memory_order_acquire)) {
        return "";
    }

    uint32_t actual_timeout = timeout_ms > 0
        ? timeout_ms : default_timeout_ms_;

    std::lock_guard<std::mutex> lock(io_mutex_);

    std::string msg = request_json + "\n";
    ssize_t written = ::write(stdin_fd_, msg.data(), msg.size());
    if (written < 0 || static_cast<size_t>(written) != msg.size()) {
        logger->error("Write to child stdin failed: {}",
                      strerror(errno));
        connected_ = false;
        return "";
    }

    return read_line(stdout_fd_, actual_timeout);
}

/**
 * @brief Check if child process is alive via kill(pid, 0).
 * @return true if connected.
 * @internal
 * @version 1.8.7
 */
bool StdioTransport::is_connected() const {
    if (!connected_) {
        return false;
    }
    if (child_pid_ > 0 && kill(child_pid_, 0) == 0) {
        return true;
    }
    return false;
}

/**
 * @brief Cancel any in-flight read by tripping cancel_flag_.
 *
 * read_line / poll_until_ready short-circuit the next poll tick so
 * the pending send_request returns empty within ~50ms (one poll
 * slice). Subsequent send_request calls also early-exit until the
 * flag is cleared implicitly by a successful open(). (P1-10)
 *
 * @internal
 * @version 2.0.6-rc16
 */
void StdioTransport::interrupt() {
    cancel_flag_.store(true, std::memory_order_release);
}

/**
 * @brief Spawn child process using posix_spawn with file actions.
 * @param stdin_r Child stdin read end (dup2'd to STDIN_FILENO).
 * @param stdout_w Child stdout write end (dup2'd to STDOUT_FILENO).
 * @param stderr_w Child stderr write end (dup2'd to STDERR_FILENO).
 * @param env_strs Merged environment strings.
 * @return true on success, child_pid_ set.
 * @utility
 * @version 1.8.7
 */
bool StdioTransport::spawn_child(
    int stdin_r, int stdout_w, int stderr_w,
    const std::vector<std::string>& env_strs) {

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdin_r, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdout_w, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderr_w, STDERR_FILENO);

    // Build argv: [command, args..., nullptr]
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(command_.c_str()));
    for (const auto& arg : args_) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    // Build envp
    std::vector<char*> envp;
    for (const auto& e : env_strs) {
        envp.push_back(const_cast<char*>(e.c_str()));
    }
    envp.push_back(nullptr);

    int err = posix_spawnp(
        &child_pid_, command_.c_str(), &actions,
        nullptr, argv.data(), envp.data());

    posix_spawn_file_actions_destroy(&actions);

    if (err != 0) {
        logger->error("posix_spawnp failed for '{}': {}",
                      command_, strerror(err));
        child_pid_ = -1;
        return false;
    }
    return true;
}

/**
 * @brief Build merged environment for child process.
 * @return Vector of "KEY=VALUE" strings.
 * @utility
 * @version 1.8.7
 */
std::vector<std::string> StdioTransport::build_env() const {
    std::map<std::string, std::string> merged;

    // Copy parent environment
    for (char** ep = environ; ep && *ep; ++ep) {
        std::string entry(*ep);
        auto eq = entry.find('=');
        if (eq != std::string::npos) {
            merged[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
    }

    // Apply overrides
    for (const auto& [key, val] : env_) {
        merged[key] = val;
    }

    std::vector<std::string> result;
    result.reserve(merged.size());
    for (const auto& [key, val] : merged) {
        result.push_back(key + "=" + val);
    }
    return result;
}

/**
 * @brief Create pipe pair and set close-on-exec on both ends.
 * @param[out] read_fd Read end.
 * @param[out] write_fd Write end.
 * @return true on success.
 * @utility
 * @version 1.8.7
 */
bool StdioTransport::create_pipe(int& read_fd, int& write_fd) {
    int fds[2];
    if (::pipe(fds) != 0) {
        return false;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    read_fd = fds[0];
    write_fd = fds[1];
    return true;
}

/**
 * @brief Poll fd for readability within remaining deadline.
 * @param fd File descriptor.
 * @param deadline Absolute deadline.
 * @return 1 if ready, 0 on timeout, -1 on error, -2 if slice expired.
 * @utility
 * @version 2.0.6-rc16
 */
int StdioTransport::poll_until_ready(
    int fd,
    std::chrono::steady_clock::time_point deadline) {

    auto remaining = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());

    if (remaining.count() <= 0) {
        return 0;
    }

    // P1-10 (2.0.6-rc16): cap the single poll slice at 100ms so the
    // caller's cancel_flag_ is re-checked at least ten times/second.
    constexpr int kSliceMs = 100;
    int slice = static_cast<int>(remaining.count());
    if (slice > kSliceMs) { slice = kSliceMs; }

    struct pollfd pfd{fd, POLLIN, 0};
    int rc = ::poll(&pfd, 1, slice);
    if (rc == 0) {
        // Slice expired; report 0 only if the full deadline passed.
        if (std::chrono::steady_clock::now() < deadline) {
            return -2;  // caller retries after checking cancel_flag
        }
    }
    return rc;
}

/**
 * @brief Read a single newline-delimited line from fd using poll.
 *
 * All early-exit paths (cancel, timeout, read error) break to a single
 * terminal `return ""` to satisfy the ≤3 return gate.  Only a complete
 * newline-terminated line returns the accumulated string.
 *
 * @param fd File descriptor.
 * @param timeout_ms Timeout.
 * @return Line without newline, or empty on error/timeout/cancel.
 * @utility
 * @version 2.0.6-rc16
 */
std::string StdioTransport::read_line(int fd, uint32_t timeout_ms) {
    std::string line;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (true) {
        // P1-10: short-circuit if the engine interrupted this request.
        if (cancel_flag_.load(std::memory_order_acquire)) {
            logger->info("Transport read cancelled by interrupt");
            break;
        }
        int ready = poll_until_ready(fd, deadline);
        if (ready == -2) { continue; }  // slice expired, re-check cancel
        if (ready <= 0) {
            if (ready == 0) { logger->warn("Read timeout after {}ms", timeout_ms); }
            break;
        }
        char ch = 0;
        if (::read(fd, &ch, 1) <= 0) { break; }
        if (ch == '\n') { return line; }
        line += ch;
    }
    return "";
}

/**
 * @brief Forward child stderr to spdlog WARNING.
 * @callback
 * @version 1.8.7
 */
void StdioTransport::stderr_reader_loop() {
    constexpr int poll_timeout_ms = 500;
    while (connected_) {
        struct pollfd pfd{stderr_fd_, POLLIN, 0};
        int ret = ::poll(&pfd, 1, poll_timeout_ms);
        if (ret <= 0) {
            continue;
        }

        char buf[1024];
        ssize_t n = ::read(stderr_fd_, buf, sizeof(buf) - 1);
        if (n <= 0) {
            break;
        }
        buf[n] = '\0';
        logger->warn("[{}] {}", command_, buf);
    }
}

/**
 * @brief Terminate and reap child process.
 * @utility
 * @version 1.8.7
 */
void StdioTransport::terminate_child() {
    if (child_pid_ <= 0) {
        return;
    }

    ::kill(child_pid_, SIGTERM);

    // Wait up to 3 seconds for graceful exit
    constexpr int max_wait_ms = 3000;
    constexpr int poll_interval_ms = 50;
    int waited = 0;
    while (waited < max_wait_ms) {
        int status = 0;
        pid_t result = ::waitpid(child_pid_, &status, WNOHANG);
        if (result == child_pid_) {
            child_pid_ = -1;
            return;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(poll_interval_ms));
        waited += poll_interval_ms;
    }

    // Force kill
    ::kill(child_pid_, SIGKILL);
    ::waitpid(child_pid_, nullptr, 0);
    logger->warn("Force-killed child PID {}", child_pid_);
    child_pid_ = -1;
}

/**
 * @brief Close a file descriptor if open, set to -1.
 * @param fd File descriptor reference.
 * @utility
 * @version 1.8.7
 */
void StdioTransport::close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

} // namespace entropic
