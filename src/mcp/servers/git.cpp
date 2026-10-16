// SPDX-License-Identifier: Apache-2.0
/**
 * @file git.cpp
 * @brief GitServer implementation — version control operations.
 * @version 1.8.5
 */

#include <entropic/mcp/servers/git.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <array>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <string>

static auto logger = entropic::log::get("mcp.git");

namespace entropic {

// ── shell helper ────────────────────────────────────────────────

/**
 * @brief Start `git -C <repo_dir> <args...>` with stdout+stderr on a pipe.
 * @param repo_dir Repository root.
 * @param args Arguments after "-C repo_dir", one element each.
 * @param[out] read_fd Read end of the child's output pipe (-1 on failure).
 * @return Child pid, or -1 when the pipe or fork failed.
 * @utility
 * @version 2.14.4
 */
static pid_t spawn_git(const std::string& repo_dir,
                       const std::vector<std::string>& args, int& read_fd) {
    std::vector<std::string> owned = {"git", "-C", repo_dir};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : owned) { argv.push_back(a.data()); }
    argv.push_back(nullptr);
    int fds[2] = {-1, -1};
    const pid_t pid = (::pipe(fds) == 0) ? ::fork() : -1;
    if (pid == 0) {
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        ::execvp("git", argv.data());
        ::_exit(127);
    }
    if (fds[1] >= 0) { ::close(fds[1]); }
    read_fd = fds[0];
    return pid;
}

/**
 * @brief Read a pipe to EOF and close it.
 * @param fd Read end (-1 = nothing to read).
 * @return Everything read.
 * @utility
 * @version 2.14.4
 */
static std::string drain_and_close(int fd) {
    std::string output;
    std::array<char, 4096> buf{};
    for (ssize_t n; fd >= 0 && (n = ::read(fd, buf.data(), buf.size())) > 0;) {
        output.append(buf.data(), static_cast<size_t>(n));
    }
    if (fd >= 0) { ::close(fd); }
    return output;
}

/**
 * @brief Run `git -C <repo_dir> <args...>` with NO shell (gh#211, v2.14.4).
 *
 * The shared execution path behind all eight git tools. Every argument is
 * one argv element handed to execvp: nothing is parsed by a shell, so a
 * value carrying `;`, `$(...)`, quotes or spaces is passed to git exactly
 * as given. Until v2.14.4 this built a command line for popen(), unquoted,
 * and a model-supplied file name, ref or commit message — or a repository
 * path with a `;` in it — ran arbitrary commands as the user. Callers put
 * `--` before path arguments and refuse ref values that start with `-`, so
 * a value cannot become an option either.
 *
 * @param repo_dir Repository root directory.
 * @param args Arguments after "git -C repo_dir", one element each.
 * @return Combined stdout+stderr and the exit code; (message, -1) when git
 *         could not be started.
 * @req REQ-MCP-023
 * @version 2.14.4
 */
static std::pair<std::string, int> run_git(
    const std::string& repo_dir,
    const std::vector<std::string>& args) {
    int fd = -1;
    const pid_t pid = spawn_git(repo_dir, args, fd);
    const std::string output = drain_and_close(fd);
    int status = 0;
    const bool reaped = pid > 0 && ::waitpid(pid, &status, 0) == pid;
    const int exit_code = (reaped && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    logger->info("Git: {} arg(s) [{}] exit={}, {} chars", args.size(),
                 args.empty() ? std::string{} : args.front(), exit_code,
                 output.size());
    return reaped ? std::make_pair(output, exit_code)
                  : std::make_pair(std::string("Failed to open git process"), -1);
}

/**
 * @brief Split a space-separated `files` argument into argv elements.
 *
 * The schemas say space-separated; this honours that without a shell.
 *
 * @param s Raw value.
 * @return One element per whitespace-separated token.
 * @utility
 * @version 2.14.4
 */
static std::vector<std::string> split_files(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string tok; in >> tok;) { out.push_back(tok); }
    return out;
}

/**
 * @brief The declared refusal for a ref value that would read as an option.
 * @param what Argument name.
 * @param value The value.
 * @return `{"error":"invalid_args",...}` ServerResponse.
 * @utility
 * @version 2.14.4
 */
static ServerResponse option_refusal(const std::string& what,
                                     const std::string& value) {
    logger->warn("[git] refused {}='{}': starts with '-'", what, value);
    return {tool_error_json("invalid_args", what + " must not start with '-' (got '"
                                + value + "')"), {}};
}

/**
 * @brief Format a git result as JSON ServerResponse.
 *
 * The single response shape all eight git tools answer in.
 *
 * @param output Combined stdout+stderr from the git process.
 * @param exit_code Process exit code.
 * @return A ServerResponse with no directives whose result is a JSON
 *         object carrying exit_code and output — git tools have no
 *         engine-level side effects.
 * @req REQ-MCP-023
 * @req REQ-MCP-002
 * @version 1.8.5
 */
static ServerResponse make_git_response(
    const std::string& output, int exit_code) {

    nlohmann::json result;
    result["exit_code"] = exit_code;
    result["output"] = output;
    return {result.dump(), {}};
}

// ── GitStatusTool ───────────────────────────────────────────────

/**
 * @brief Tool: git status --short.
 * @dg_internal
 * @version 1.8.5
 */
class GitStatusTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and repo dir ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @dg_internal
     * @version 1.8.5
     */
    GitStatusTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     *
     * status/diff/log inspect the repo without mutating it, so they
     * relax the WRITE default; add/commit/branch/checkout/reset keep it.
     *
     * @return MCPAccessLevel::READ.
     * @req REQ-MCP-011
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Run git status --short.
     * @param args_json Unused.
     * @return ServerResponse with status output.
     * @dg_internal
     * @version 2.14.4
     */
    ServerResponse execute(const std::string& args_json) override {
        (void)args_json;
        auto [out, rc] = run_git(
            server_.repo_dir().string(), {"status", "--short"});
        logger->info("[git.status] exit={}", rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitDiffTool ─────────────────────────────────────────────────

/**
 * @brief Tool: git diff [--staged] [file].
 * @dg_internal
 * @version 1.8.5
 */
class GitDiffTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @dg_internal
     * @version 1.8.5
     */
    GitDiffTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool only inspects the repo.
     * @req REQ-MCP-011
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Run git diff with optional flags.
     * @param args_json JSON with optional "staged" and "file".
     * @return ServerResponse with diff output.
     * @dg_internal
     * @version 2.14.4
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        std::vector<std::string> cmd = {"diff"};
        if (args.value("staged", false)) {
            cmd.push_back("--staged");
        }
        if (args.contains("file")) {
            cmd.push_back("--");  // gh#211: a pathspec, never an option
            cmd.push_back(args["file"].get<std::string>());
        }
        auto [out, rc] = run_git(server_.repo_dir().string(), cmd);
        logger->info("[git.diff] exit={}", rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitLogTool ──────────────────────────────────────────────────

/**
 * @brief Tool: git log -N [--oneline].
 * @dg_internal
 * @version 1.8.5
 */
class GitLogTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @dg_internal
     * @version 1.8.5
     */
    GitLogTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool only inspects the repo.
     * @req REQ-MCP-011
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Run git log with count and format options.
     * @param args_json JSON with optional "count" and "oneline".
     * @return ServerResponse with log output.
     * @dg_internal
     * @version 2.14.4
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        int count = args.value("count", 10);
        std::vector<std::string> cmd = {"log", "-" + std::to_string(count)};
        if (args.value("oneline", false)) {
            cmd.push_back("--oneline");
        }
        auto [out, rc] = run_git(server_.repo_dir().string(), cmd);
        logger->info("[git.log] exit={}", rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitCommitTool ───────────────────────────────────────────────

/**
 * @brief Tool: git commit -m "message", optionally git add -A first.
 * @dg_internal
 * @version 1.8.5
 */
class GitCommitTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @dg_internal
     * @version 1.8.5
     */
    GitCommitTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Run git commit with optional add-all.
     * @param args_json JSON with "message" and optional "add_all".
     * @return ServerResponse with commit output.
     * @dg_internal
     * @version 2.14.4
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        auto repo = server_.repo_dir().string();

        if (args.value("add_all", false)) {
            auto [out, rc] = run_git(repo, {"add", "-A"});
            if (rc != 0) {
                logger->error("[git.commit] add -A failed: {}", out);
                return make_git_response(out, rc);
            }
        }

        std::string msg = args.at("message").get<std::string>();
        // gh#211: the message is ONE argv element — quotes, $() and
        // backticks in it are just text.
        auto [out, rc] = run_git(repo, {"commit", "-m", msg});
        logger->info("[git.commit] exit={}", rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitBranchTool ───────────────────────────────────────────────

/**
 * @brief Tool: git branch -a, or git checkout -b name.
 * @dg_internal
 * @version 1.8.5
 */
class GitBranchTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @dg_internal
     * @version 1.8.5
     */
    GitBranchTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief List branches or create a new one.
     * @param args_json JSON with optional "create" branch name.
     * @return ServerResponse with branch output.
     * @dg_internal
     * @version 2.14.4
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        // v2.14.4: `name`, as branch.json has always declared — the code read
        // `create`, so a model following the schema could never create one.
        std::vector<std::string> cmd = {"branch", "-a"};
        if (args.contains("name")) {
            const auto name = args["name"].get<std::string>();
            if (!name.empty() && name.front() == '-') {
                return option_refusal("name", name);  // gh#211
            }
            cmd = {"checkout", "-b", name};
        }
        auto [out, rc] = run_git(server_.repo_dir().string(), cmd);
        logger->info("[git.branch] exit={}", rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitCheckoutTool ─────────────────────────────────────────────

/**
 * @brief Tool: git checkout target.
 * @dg_internal
 * @version 1.8.5
 */
class GitCheckoutTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @dg_internal
     * @version 1.8.5
     */
    GitCheckoutTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Checkout a branch or commit.
     * @param args_json JSON with "target".
     * @return ServerResponse with checkout output.
     * @dg_internal
     * @version 2.14.4
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        std::string target = args.at("target").get<std::string>();
        if (!target.empty() && target.front() == '-') {
            return option_refusal("target", target);  // gh#211
        }
        auto [out, rc] = run_git(server_.repo_dir().string(), {"checkout", target});
        logger->info("[git.checkout] target='{}' exit={}", target, rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitAddTool ──────────────────────────────────────────────────

/**
 * @brief Tool: git add files (space-separated).
 * @dg_internal
 * @version 1.8.5
 */
class GitAddTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @dg_internal
     * @version 1.8.5
     */
    GitAddTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Stage files for commit.
     * @param args_json JSON with "files" (space-separated string).
     * @return ServerResponse with add output.
     * @dg_internal
     * @version 2.14.4
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        std::string files = args.at("files").get<std::string>();
        std::vector<std::string> cmd = {"add", "--"};  // gh#211: paths only
        for (auto& f : split_files(files)) { cmd.push_back(std::move(f)); }
        auto [out, rc] = run_git(server_.repo_dir().string(), cmd);
        logger->info("[git.add] files='{}' exit={}", files, rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitResetTool ────────────────────────────────────────────────

/**
 * @brief Tool: git reset HEAD [files].
 * @dg_internal
 * @version 1.8.5
 */
class GitResetTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @dg_internal
     * @version 1.8.5
     */
    GitResetTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Unstage files.
     * @param args_json JSON with optional "files".
     * @return ServerResponse with reset output.
     * @dg_internal
     * @version 2.14.4
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        std::vector<std::string> cmd = {"reset", "HEAD", "--"};  // gh#211
        if (args.contains("files")) {
            for (auto& f : split_files(args["files"].get<std::string>())) {
                cmd.push_back(std::move(f));
            }
        }
        auto [out, rc] = run_git(server_.repo_dir().string(), cmd);
        logger->info("[git.reset] exit={}", rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitServer ───────────────────────────────────────────────────

/**
 * @brief Construct with repo directory and data dir.
 *
 * A thin MCPServerBase subclass: build the eight tools, register them,
 * and override only set_working_dir. Every tool runs against repo_dir_.
 *
 * @param repo_dir Repository root directory.
 * @param data_dir Path to bundled data directory.
 * @req REQ-MCP-001
 * @req REQ-MCP-023
 * @version 2.3.7
 */
GitServer::GitServer(
    const std::filesystem::path& repo_dir,
    const std::string& data_dir)
    : MCPServerBase("git")
    , repo_dir_(repo_dir) {

    create_git_tools(data_dir + "/tools");
    register_git_tools();

    logger->info("GitServer initialized: repo='{}'",
                 repo_dir_.string());
}

/**
 * @brief Construct the eight git tool instances (ctor step 1).
 *
 * status, diff, log, commit, branch, checkout, add and reset — the
 * server's whole surface, each built from its bundled JSON descriptor.
 *
 * @param tools_dir Directory holding the tool JSON definitions.
 * @req REQ-MCP-023
 * @version 2.3.7
 */
void GitServer::create_git_tools(const std::string& tools_dir) {
    status_ = std::make_unique<GitStatusTool>(
        load_tool_definition("status", "git", tools_dir), *this);
    diff_ = std::make_unique<GitDiffTool>(
        load_tool_definition("diff", "git", tools_dir), *this);
    log_ = std::make_unique<GitLogTool>(
        load_tool_definition("log", "git", tools_dir), *this);
    commit_ = std::make_unique<GitCommitTool>(
        load_tool_definition("commit", "git", tools_dir), *this);
    branch_ = std::make_unique<GitBranchTool>(
        load_tool_definition("branch", "git", tools_dir), *this);
    checkout_ = std::make_unique<GitCheckoutTool>(
        load_tool_definition("checkout", "git", tools_dir), *this);
    add_ = std::make_unique<GitAddTool>(
        load_tool_definition("add", "git", tools_dir), *this);
    reset_ = std::make_unique<GitResetTool>(
        load_tool_definition("reset", "git", tools_dir), *this);
}

/**
 * @brief Register the eight git tools with the base server (step 2).
 *
 * Registration is all it takes — dispatch, envelope shape and anchoring
 * come from MCPServerBase.
 *
 * @req REQ-MCP-001
 * @version 2.3.7
 */
void GitServer::register_git_tools() {
    register_tool(status_.get());
    register_tool(diff_.get());
    register_tool(log_.get());
    register_tool(commit_.get());
    register_tool(branch_.get());
    register_tool(checkout_.get());
    register_tool(add_.get());
    register_tool(reset_.get());
}

/**
 * @brief Destructor.
 * @dg_internal
 * @version 1.8.5
 */
GitServer::~GitServer() = default;

/**
 * @brief Set working directory (repo root).
 *
 * Re-targets every git tool at once so a sandbox swap does not require
 * reconstructing the server.
 *
 * @param path New repo directory.
 * @return true — the re-target always succeeds.
 * @req REQ-MCP-023
 * @version 1.8.5
 */
bool GitServer::set_working_dir(const std::string& path) {
    repo_dir_ = path;
    logger->info("Repo directory set to: {}", path);
    return true;
}

/**
 * @brief Get repo directory.
 * @return The repo root every git tool runs `git -C` against.
 * @req REQ-MCP-023
 * @version 1.8.5
 */
const std::filesystem::path& GitServer::repo_dir() const {
    return repo_dir_;
}

} // namespace entropic
