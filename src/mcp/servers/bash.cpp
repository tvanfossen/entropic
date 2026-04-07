/**
 * @file bash.cpp
 * @brief BashServer implementation — shell command execution with safety.
 * @version 1.8.5
 */

#include <entropic/mcp/servers/bash.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

static auto logger = entropic::log::get("mcp.bash");

namespace entropic {

// ── dangerous command patterns ──────────────────────────────────

/**
 * @brief Check if a command matches a dangerous pattern.
 * @param cmd Command string to check.
 * @return true if the command is blocked.
 * @internal
 * @version 1.8.5
 */
static bool is_dangerous(const std::string& cmd) {
    static const std::array<std::string, 6> blocked = {
        "rm -rf /",
        "rm -rf /*",
        "dd if=/dev/zero",
        ":(){:|:&};:",
        "> /dev/sda",
        "mkfs",
    };
    for (const auto& pattern : blocked) {
        if (cmd.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check additional prefix patterns for dangerous commands.
 * @param cmd Command string to check.
 * @return true if the command matches a dangerous prefix.
 * @internal
 * @version 1.8.5
 */
static bool matches_dangerous_prefix(const std::string& cmd) {
    static const std::array<std::string, 3> prefixes = {
        "> /dev/",
        "mkfs.",
        "dd if=",
    };
    for (const auto& prefix : prefixes) {
        if (cmd.find(prefix) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Run a shell command and capture output.
 * @param full_cmd Command string including cd prefix.
 * @return Pair of output string and exit code.
 * @internal
 * @version 1.8.5
 */
static std::pair<std::string, int> run_popen(
    const std::string& full_cmd) {

    FILE* pipe = popen(full_cmd.c_str(), "r");  // NOLINT
    if (pipe == nullptr) {
        return {"Failed to open process", -1};
    }

    std::string output;
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), buf.size(), pipe) != nullptr) {
        output += buf.data();
    }

    int status = pclose(pipe);  // NOLINT
    int exit_code = WEXITSTATUS(status);
    return {output, exit_code};
}

// ── ExecuteTool ─────────────────────────────────────────────────

/**
 * @brief Tool for executing shell commands.
 * @internal
 * @version 1.8.5
 */
class ExecuteTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition with server ref.
     * @param def Tool definition loaded from JSON.
     * @param server Owning BashServer reference.
     * @internal
     * @version 1.8.5
     */
    ExecuteTool(ToolDefinition def, BashServer& server)
        : ToolBase(std::move(def)), server_(server) {}

    /**
     * @brief Execute a shell command.
     * @param args_json JSON with "command" and optional "working_dir".
     * @return ServerResponse with stdout/stderr or error.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    BashServer& server_;
};

/**
 * @brief Execute a shell command after safety checks.
 * @param args_json JSON with "command" and optional "working_dir".
 * @return ServerResponse with output or error.
 * @internal
 * @version 2.0.0
 */
ServerResponse ExecuteTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string command = args.at("command").get<std::string>();

    std::string cwd = args.value(
        "working_dir", server_.working_dir().string());

    logger->info("[bash.execute] cmd='{}' cwd='{}'", command, cwd);

    if (is_dangerous(command) || matches_dangerous_prefix(command)) {
        logger->warn("Blocked dangerous command: {}", command);
        return {"Error: command blocked by safety filter", {}};
    }

    std::string full_cmd =
        "cd " + cwd + " && " + command + " 2>&1";

    auto [output, exit_code] = run_popen(full_cmd);
    logger->info("Bash: exit={}, stdout={} chars, cmd='{}'",
                 exit_code, output.size(), command);

    nlohmann::json result;
    result["exit_code"] = exit_code;
    result["output"] = output;
    return {result.dump(), {}};
}

// ── BashServer ──────────────────────────────────────────────────

/**
 * @brief Construct with working directory and data dir.
 * @param working_dir Default working directory for commands.
 * @param data_dir Path to bundled data directory.
 * @param timeout Command timeout in seconds.
 * @internal
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
 * @internal
 * @version 1.8.5
 */
BashServer::~BashServer() = default;

/**
 * @brief Permission pattern: "execute:{base_cmd} *".
 * @param tool_name Tool name.
 * @param args_json Arguments JSON.
 * @return Permission pattern with base command extracted.
 * @internal
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
 * @param path New working directory.
 * @return true on success.
 * @internal
 * @version 1.8.5
 */
bool BashServer::set_working_dir(const std::string& path) {
    working_dir_ = path;
    logger->info("Working directory set to: {}", path);
    return true;
}

/**
 * @brief Get the working directory.
 * @return Working directory path.
 * @internal
 * @version 1.8.5
 */
const std::filesystem::path& BashServer::working_dir() const {
    return working_dir_;
}

/**
 * @brief Get command timeout.
 * @return Timeout in seconds.
 * @internal
 * @version 1.8.5
 */
int BashServer::timeout() const {
    return timeout_;
}

} // namespace entropic
