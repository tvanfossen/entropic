// SPDX-License-Identifier: Apache-2.0
/**
 * @file bash.cpp
 * @brief BashServer implementation — shell command execution under an
 *        enforced timeout, one process group per command.
 * @version 2.13.0
 */

#include <entropic/mcp/servers/bash.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;  // NOLINT — POSIX, for posix_spawn

static auto logger = entropic::log::get("mcp.bash");

namespace entropic {

// ── working_dir validation ──────────────────────────────────────

/**
 * @brief Reject working_dir values that would smuggle shell syntax.
 *
 * The bash tool's security model is operator approval (the engine's
 * tool-call gate), not pattern matching against the command. But
 * `working_dir` is concatenated into a shell `cd` clause, so any
 * shell metacharacter in cwd would escape the approval surface (the
 * operator approves `command`, not the constructed shell string).
 * This check rejects the obvious smuggling vectors and requires the
 * cwd be an existing directory.
 *
 * @param cwd Caller-supplied working directory string.
 * @return true only when cwd contains none of `;&|`$<>`, newline,
 *         quotes, globs or brackets AND names an existing directory;
 *         false otherwise, which rejects the call before any shell runs.
 * @req REQ-MCP-023
 * @version 2.1.1-rc1
 */
static bool is_safe_cwd(const std::string& cwd) {
    static constexpr const char* unsafe_chars = ";&|`$<>\n\r\\\"'*?(){}[]";
    if (cwd.find_first_of(unsafe_chars) != std::string::npos) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_directory(cwd, ec);
}

// ── Running one command (v2.13.0) ───────────────────────────────
//
// popen() gave no handle on the process: `timeout_` was stored and never
// read, `sleep 30` held the tool call (and the run thread under it) for
// 30 s, a background child that inherited stdout kept the read loop open
// for its whole life, and one that did not simply outlived the call. The
// command now leads its OWN process group, so the whole tree can be killed
// as one: on the deadline, and — for anything left running — when the
// shell exits.

namespace {

using Clock = std::chrono::steady_clock;

/// @brief Read-back allowance after the group is killed, for output
/// already in the pipe. A writer OUTSIDE the group (a command that called
/// setsid) can hold the pipe open forever; this bounds the wait for it.
constexpr std::chrono::milliseconds kDrainGrace{2000};

/// @brief Poll granularity while the command runs (ms).
constexpr int kPollMs = 50;

/**
 * @brief One command's outcome.
 * @dg_internal
 * @version 2.13.0
 */
struct ShellRun {
    std::string output;        ///< Combined stdout+stderr (partial on timeout)
    int exit_code = -1;        ///< Shell status; 128+N if killed by signal N
    bool timed_out = false;    ///< The deadline expired and the group was killed
    long long elapsed_ms = 0;  ///< Wall time, spawn to reap
};

/**
 * @brief Spawn `/bin/sh -c cmd` as the leader of a NEW process group.
 *
 * stdout AND stderr go to one pipe — popen captured stderr only where the
 * command's own trailing `2>&1` reached, so a non-final command's stderr
 * went to the host's terminal. stdin is /dev/null: a tool call is not
 * interactive, and must not read the host's terminal. The pipe is
 * O_CLOEXEC so a child another thread spawns concurrently cannot inherit
 * (and hold open) this command's write end.
 *
 * @param cmd Full shell command.
 * @param[out] read_fd The parent's read end, or -1 on failure.
 * @return The child pid — also its process-group id — or -1 on failure.
 * @req REQ-MCP-023
 * @version 2.13.0
 */
pid_t spawn_shell(const std::string& cmd, int& read_fd) {
    int fds[2] = {-1, -1};
    read_fd = -1;
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        logger->error("bash: pipe2 failed: {}", std::strerror(errno));
        return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                     O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);  // pgid = the child's own pid
    const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
    pid_t pid = -1;
    int rc = ::posix_spawn(&pid, "/bin/sh", &actions, &attr,
                           const_cast<char* const*>(argv), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (rc != 0) {
        logger->error("bash: posix_spawn failed: {}", std::strerror(rc));
        ::close(fds[0]);
        pid = -1;
    }
    read_fd = pid > 0 ? fds[0] : -1;
    return pid;
}

/**
 * @brief Read whatever the pipe holds, waiting at most `wait_ms`.
 * @param fd Read end.
 * @param out Output accumulator.
 * @param wait_ms poll() timeout.
 * @return false once the pipe is at EOF (every writer is gone).
 * @req REQ-MCP-023
 * @version 2.13.0
 */
bool pump(int fd, std::string& out, int wait_ms) {
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, wait_ms) <= 0) {
        return true;  // nothing yet (or EINTR): still open
    }
    std::array<char, 4096> buf{};
    ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n > 0) {
        out.append(buf.data(), static_cast<size_t>(n));
    }
    return n > 0 || (n < 0 && (errno == EINTR || errno == EAGAIN));
}

/**
 * @brief Has the shell exited? Leaves it a ZOMBIE (WNOWAIT).
 *
 * Not reaping here is deliberate: the shell's pid is also its
 * process-group id, and an unreaped pid cannot be reused — so the group
 * kill that follows cannot hit an unrelated process. ECHILD (a host that
 * set SIGCHLD to SIG_IGN, so the kernel reaped it already) counts as
 * exited rather than spinning to the deadline.
 *
 * @param pid The shell.
 * @return true when it has exited.
 * @req REQ-MCP-023
 * @version 2.13.0
 */
bool shell_exited(pid_t pid) {
    siginfo_t info{};
    int rc = ::waitid(P_PID, static_cast<id_t>(pid), &info,
                      WEXITED | WNOHANG | WNOWAIT);
    return (rc == 0 && info.si_pid == pid) || (rc == -1 && errno == ECHILD);
}

/**
 * @brief Collect output until the shell exits or the deadline passes.
 * @param pid The shell.
 * @param fd Read end.
 * @param deadline When the command's time is up.
 * @param out Output accumulator.
 * @return true when the deadline passed first.
 * @req REQ-MCP-023
 * @version 2.13.0
 */
bool collect_until_exit(pid_t pid, int fd, Clock::time_point deadline,
                        std::string& out) {
    bool open = true;
    bool exited = false;
    bool expired = false;
    while (!exited && !expired) {
        if (open) {
            open = pump(fd, out, kPollMs);
        } else {
            // The shell closed its output but is still running.
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
        exited = shell_exited(pid);
        expired = !exited && Clock::now() >= deadline;
    }
    return expired;
}

/**
 * @brief Read what the pipe still holds once the group is dead.
 *
 * Bounded by kDrainGrace: only a process that left the group can still
 * hold the write end, and it is not ours to wait for.
 *
 * @param fd Read end.
 * @param out Output accumulator.
 * @req REQ-MCP-023
 * @version 2.13.0
 */
void drain(int fd, std::string& out) {
    const auto until = Clock::now() + kDrainGrace;
    bool open = true;
    while (open && Clock::now() < until) {
        open = pump(fd, out, kPollMs);
    }
    if (open) {
        logger->warn("bash: output pipe still open {} ms after the process "
                     "group was killed — a process that left the group "
                     "holds it; returning without its output",
                     kDrainGrace.count());
    }
}

/**
 * @brief Reap the shell and translate its status.
 * @param pid The shell.
 * @return Exit status; 128+N when killed by signal N (the shell
 *         convention); -1 when it could not be reaped.
 * @req REQ-MCP-023
 * @version 2.13.0
 */
int reap(pid_t pid) {
    int status = 0;
    pid_t rc = -1;
    do {
        rc = ::waitpid(pid, &status, 0);
    } while (rc == -1 && errno == EINTR);
    int code = -1;
    if (rc == pid && WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (rc == pid && WIFSIGNALED(status)) {
        code = 128 + WTERMSIG(status);
    }
    return code;
}

/**
 * @brief Run one command to completion or to its deadline.
 *
 * Whichever comes first, the command's process group is SIGKILLed before
 * returning: on the deadline that stops the command; on a normal exit it
 * stops whatever the command left running in the background. The shell
 * is then reaped, so nothing is left behind — no orphans, no zombies.
 *
 * @param cmd Full shell command.
 * @param timeout_s Wall-clock limit in seconds.
 * @return The run's output, exit code, and whether it timed out.
 * @req REQ-MCP-023
 * @version 2.13.0
 */
ShellRun run_shell(const std::string& cmd, int timeout_s) {
    ShellRun run;
    const auto start = Clock::now();
    int fd = -1;
    const pid_t pid = spawn_shell(cmd, fd);
    if (pid < 0) {
        run.output = "Failed to start /bin/sh";
        return run;
    }
    run.timed_out = collect_until_exit(
        pid, fd, start + std::chrono::seconds(timeout_s), run.output);
    ::kill(-pid, SIGKILL);  // the whole group: timed-out tree or leftovers
    drain(fd, run.output);
    ::close(fd);
    run.exit_code = reap(pid);
    run.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start).count();
    return run;
}

/**
 * @brief The typed error the model sees when a command hits the limit.
 *
 * Names the limit AND the elapsed time, keeps the partial output, and
 * says what to do instead — a bare "timed out" invites the model to
 * retry the same command.
 *
 * @param run The timed-out run.
 * @param limit_s The enforced limit.
 * @return `{"error":"timeout","message",...,"timeout_seconds",
 *         "elapsed_ms","output"}`.
 * @req REQ-MCP-023
 * @version 2.13.0
 */
nlohmann::json timeout_error(const ShellRun& run, int limit_s) {
    std::array<char, 32> secs{};
    std::snprintf(secs.data(), secs.size(), "%.1f",
                  static_cast<double>(run.elapsed_ms) / 1000.0);
    nlohmann::json err;
    err["error"] = "timeout";
    err["message"] = "Command exceeded the bash tool's "
        + std::to_string(limit_s) + " s timeout (ran " + secs.data()
        + " s) and was killed, together with every process it started. "
          "Output printed before the kill is in `output`. Split the work "
          "into shorter commands; the limit is set by the operator "
          "(mcp.bash.timeout_seconds).";
    err["timeout_seconds"] = limit_s;
    err["elapsed_ms"] = run.elapsed_ms;
    err["output"] = run.output;
    return err;
}

}  // namespace

// ── ExecuteTool ─────────────────────────────────────────────────

/**
 * @brief Tool for executing shell commands.
 * @dg_internal
 * @version 1.8.5
 */
class ExecuteTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition with server ref.
     * @param def Tool definition loaded from JSON.
     * @param server Owning BashServer reference.
     * @dg_internal
     * @version 1.8.5
     */
    ExecuteTool(ToolDefinition def, BashServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Execute a shell command.
     * @param args_json JSON with "command" and optional "working_dir".
     * @return ServerResponse with stdout/stderr or error.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    BashServer& server_;
};

/**
 * @brief Execute a shell command. Operator approval is the gate.
 *
 * Security model: the engine's tool-call approval flow gates whether
 * this method runs at all. There is no command-content allowlist or
 * denylist here; the operator approves a specific `command` value
 * shown in the prompt, and that's what runs. The only validation we
 * do is on `working_dir`, because that field is concatenated into a
 * shell `cd` clause and could smuggle commands the operator never
 * saw.
 *
 * v2.13.0: the server's timeout is ENFORCED — see run_shell. A command
 * that hits it returns the typed `timeout` error instead of an exit code.
 *
 * @param args_json JSON with "command" and optional "working_dir"
 *                  (defaulting to the server's own working directory).
 * @return A ServerResponse with no directives whose result is a JSON
 *         object carrying the process exit_code and combined
 *         stdout+stderr output; or the typed `timeout` error (limit,
 *         elapsed, partial output) when the command outlived the limit;
 *         or the working_dir rejection error when the cwd failed
 *         validation — in which case no shell ran.
 * @req REQ-MCP-023
 * @version 2.13.0
 */
ServerResponse ExecuteTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string command = args.at("command").get<std::string>();

    std::string cwd = args.value(
        "working_dir", server_.working_dir().string());

    logger->info("[bash.execute] cmd='{}' cwd='{}'", command, cwd);

    if (!is_safe_cwd(cwd)) {
        logger->warn("Rejected unsafe working_dir: '{}'", cwd);
        return {"Error: working_dir is not an existing directory or "
                "contains shell metacharacters", {}};
    }

    std::string full_cmd =
        "cd " + cwd + " && " + command + " 2>&1";

    const int limit = server_.timeout();
    auto run = run_shell(full_cmd, limit);
    nlohmann::json result;
    if (run.timed_out) {
        logger->warn("Bash: TIMEOUT after {} ms (limit {} s) — process "
                     "group killed, cmd='{}'",
                     run.elapsed_ms, limit, command);
        result = timeout_error(run, limit);
    } else {
        logger->info("Bash: exit={}, output={} chars, {} ms, cmd='{}'",
                     run.exit_code, run.output.size(), run.elapsed_ms,
                     command);
        result["exit_code"] = run.exit_code;
        result["output"] = run.output;
    }
    return {result.dump(), {}};
}

// ── BashServer ──────────────────────────────────────────────────

/**
 * @brief Construct with working directory and data dir.
 *
 * A thin MCPServerBase subclass: it builds its one tool, registers it,
 * and overrides only get_permission_pattern and set_working_dir.
 *
 * @param working_dir Default working directory for commands.
 * @param data_dir Path to bundled data directory.
 * @param timeout Command timeout in seconds.
 * @req REQ-MCP-001
 * @req REQ-MCP-023
 * @version 1.8.5
 */
BashServer::BashServer(
    const std::filesystem::path& working_dir,
    const std::string& data_dir,
    int timeout)
    : MCPServerBase("bash")
    , working_dir_(working_dir)
    , timeout_(timeout) {

    auto def = load_tool_definition(
        "execute", "bash", data_dir + "/tools");

    execute_tool_ = std::make_unique<ExecuteTool>(
        std::move(def), *this);

    register_tool(execute_tool_.get());

    logger->info("BashServer initialized: cwd='{}' timeout={}s",
                 working_dir_.string(), timeout_);
}

/**
 * @brief Destructor.
 * @dg_internal
 * @version 1.8.5
 */
BashServer::~BashServer() = default;

/**
 * @brief Permission pattern: "execute:{base_cmd} *".
 *
 * Approval granularity matters here: keying on the base command means
 * an operator's "always allow" applies to a command family rather than
 * to every shell invocation.
 *
 * @param tool_name Tool name.
 * @param args_json Arguments JSON.
 * @return "<tool>:<base command> *" — for `python -m x`, the pattern
 *         names `python`. Unparseable arguments degrade to a base
 *         command of "unknown" (logged), never a wildcard.
 * @req REQ-MCP-023
 * @req REQ-MCP-009
 * @version 1.8.5
 */
std::string
BashServer::get_permission_pattern(const std::string& tool_name, const std::string& args_json) const {

    std::string base_cmd = "unknown";
    try {
        auto args = nlohmann::json::parse(args_json);
        std::string cmd = args.at("command").get<std::string>();
        auto space = cmd.find(' ');
        base_cmd = (space != std::string::npos)
            ? cmd.substr(0, space) : cmd;
    } catch (const std::exception& e) {
        logger->warn("Failed to parse command for permission: {}",
                     e.what());
    }
    return tool_name + ":" + base_cmd + " *";
}

/**
 * @brief Set the working directory.
 *
 * Re-targets the server so a sandbox swap does not require
 * reconstructing it.
 *
 * @param path New working directory.
 * @return true — the re-target always succeeds; the path itself is
 *         validated per call by is_safe_cwd.
 * @req REQ-MCP-023
 * @version 1.8.5
 */
bool BashServer::set_working_dir(const std::string& path) {
    working_dir_ = path;
    logger->info("Working directory set to: {}", path);
    return true;
}

/**
 * @brief Get the working directory.
 * @return The directory commands default to when a call omits
 *         `working_dir`.
 * @req REQ-MCP-023
 * @version 1.8.5
 */
const std::filesystem::path& BashServer::working_dir() const {
    return working_dir_;
}

/**
 * @brief Get command timeout.
 * @return The enforced per-command timeout in seconds.
 * @req REQ-MCP-023
 * @version 1.8.5
 */
int BashServer::timeout() const {
    return timeout_;
}

} // namespace entropic
