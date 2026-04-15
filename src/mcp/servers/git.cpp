// SPDX-License-Identifier: LGPL-3.0-or-later
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
#include <cstdio>
#include <string>

static auto logger = entropic::log::get("mcp.git");

namespace entropic {

// ── shell helper ────────────────────────────────────────────────

/**
 * @brief Run a git command in a given directory via popen.
 * @param repo_dir Repository root directory.
 * @param git_args Arguments appended after "git -C repo_dir".
 * @return Pair of output string and exit code.
 * @internal
 * @version 2.0.0
 */
static std::pair<std::string, int> run_git(
    const std::string& repo_dir,
    const std::string& git_args) {

    std::string full_cmd =
        "git -C " + repo_dir + " " + git_args + " 2>&1";

    FILE* pipe = popen(full_cmd.c_str(), "r");  // NOLINT
    if (pipe == nullptr) {
        return {"Failed to open git process", -1};
    }

    std::string output;
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), buf.size(), pipe) != nullptr) {
        output += buf.data();
    }

    int status = pclose(pipe);  // NOLINT
    int exit_code = WEXITSTATUS(status);
    logger->info("Git: '{}' exit={}, {} chars",
                 git_args, exit_code, output.size());
    return {output, exit_code};
}

/**
 * @brief Format a git result as JSON ServerResponse.
 * @param output Command output.
 * @param exit_code Process exit code.
 * @return ServerResponse with JSON result.
 * @internal
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
 * @internal
 * @version 1.8.5
 */
class GitStatusTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and repo dir ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @internal
     * @version 1.8.5
     */
    GitStatusTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ.
     * @utility
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Run git status --short.
     * @param args_json Unused.
     * @return ServerResponse with status output.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        (void)args_json;
        auto [out, rc] = run_git(
            server_.repo_dir().string(), "status --short");
        logger->info("[git.status] exit={}", rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitDiffTool ─────────────────────────────────────────────────

/**
 * @brief Tool: git diff [--staged] [file].
 * @internal
 * @version 1.8.5
 */
class GitDiffTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @internal
     * @version 1.8.5
     */
    GitDiffTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ.
     * @utility
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Run git diff with optional flags.
     * @param args_json JSON with optional "staged" and "file".
     * @return ServerResponse with diff output.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        std::string cmd = "diff";
        if (args.value("staged", false)) {
            cmd += " --staged";
        }
        if (args.contains("file")) {
            cmd += " " + args["file"].get<std::string>();
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
 * @internal
 * @version 1.8.5
 */
class GitLogTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @internal
     * @version 1.8.5
     */
    GitLogTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ.
     * @utility
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Run git log with count and format options.
     * @param args_json JSON with optional "count" and "oneline".
     * @return ServerResponse with log output.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        int count = args.value("count", 10);
        std::string cmd = "log -" + std::to_string(count);
        if (args.value("oneline", false)) {
            cmd += " --oneline";
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
 * @internal
 * @version 1.8.5
 */
class GitCommitTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @internal
     * @version 1.8.5
     */
    GitCommitTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Run git commit with optional add-all.
     * @param args_json JSON with "message" and optional "add_all".
     * @return ServerResponse with commit output.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        auto repo = server_.repo_dir().string();

        if (args.value("add_all", false)) {
            auto [out, rc] = run_git(repo, "add -A");
            if (rc != 0) {
                logger->error("[git.commit] add -A failed: {}", out);
                return make_git_response(out, rc);
            }
        }

        std::string msg = args.at("message").get<std::string>();
        std::string cmd = "commit -m \"" + msg + "\"";
        auto [out, rc] = run_git(repo, cmd);
        logger->info("[git.commit] exit={}", rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitBranchTool ───────────────────────────────────────────────

/**
 * @brief Tool: git branch -a, or git checkout -b name.
 * @internal
 * @version 1.8.5
 */
class GitBranchTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @internal
     * @version 1.8.5
     */
    GitBranchTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief List branches or create a new one.
     * @param args_json JSON with optional "create" branch name.
     * @return ServerResponse with branch output.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        std::string cmd = "branch -a";
        if (args.contains("create")) {
            cmd = "checkout -b "
                + args["create"].get<std::string>();
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
 * @internal
 * @version 1.8.5
 */
class GitCheckoutTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @internal
     * @version 1.8.5
     */
    GitCheckoutTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Checkout a branch or commit.
     * @param args_json JSON with "target".
     * @return ServerResponse with checkout output.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        std::string target = args.at("target").get<std::string>();
        std::string cmd = "checkout " + target;
        auto [out, rc] = run_git(server_.repo_dir().string(), cmd);
        logger->info("[git.checkout] target='{}' exit={}", target, rc);
        return make_git_response(out, rc);
    }

private:
    GitServer& server_;
};

// ── GitAddTool ──────────────────────────────────────────────────

/**
 * @brief Tool: git add files (space-separated).
 * @internal
 * @version 1.8.5
 */
class GitAddTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @internal
     * @version 1.8.5
     */
    GitAddTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Stage files for commit.
     * @param args_json JSON with "files" (space-separated string).
     * @return ServerResponse with add output.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        std::string files = args.at("files").get<std::string>();
        std::string cmd = "add " + files;
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
 * @internal
 * @version 1.8.5
 */
class GitResetTool : public ToolBase {
public:
    /**
     * @brief Construct with definition and server ref.
     * @param def Tool definition.
     * @param server Owning GitServer.
     * @internal
     * @version 1.8.5
     */
    GitResetTool(ToolDefinition def, GitServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Unstage files.
     * @param args_json JSON with optional "files".
     * @return ServerResponse with reset output.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        std::string cmd = "reset HEAD";
        if (args.contains("files")) {
            cmd += " " + args["files"].get<std::string>();
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
 * @param repo_dir Repository root directory.
 * @param data_dir Path to bundled data directory.
 * @internal
 * @version 1.8.5
 */
GitServer::GitServer(
    const std::filesystem::path& repo_dir,
    const std::string& data_dir)
    : MCPServerBase("git")
    , repo_dir_(repo_dir) {

    std::string tools_dir = data_dir + "/tools";

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

    register_tool(status_.get());
    register_tool(diff_.get());
    register_tool(log_.get());
    register_tool(commit_.get());
    register_tool(branch_.get());
    register_tool(checkout_.get());
    register_tool(add_.get());
    register_tool(reset_.get());

    logger->info("GitServer initialized: repo='{}'",
                 repo_dir_.string());
}

/**
 * @brief Destructor.
 * @internal
 * @version 1.8.5
 */
GitServer::~GitServer() = default;

/**
 * @brief Set working directory (repo root).
 * @param path New repo directory.
 * @return true on success.
 * @internal
 * @version 1.8.5
 */
bool GitServer::set_working_dir(const std::string& path) {
    repo_dir_ = path;
    logger->info("Repo directory set to: {}", path);
    return true;
}

/**
 * @brief Get repo directory.
 * @return Repo root path.
 * @internal
 * @version 1.8.5
 */
const std::filesystem::path& GitServer::repo_dir() const {
    return repo_dir_;
}

} // namespace entropic
