// SPDX-License-Identifier: Apache-2.0
/**
 * @file bash.h
 * @brief Bash MCP server — shell command execution under a timeout.
 * @version 2.13.0
 */

#pragma once

#include <entropic/mcp/server_base.h>

#include <filesystem>
#include <memory>
#include <string>

namespace entropic {

class ExecuteTool;

/**
 * @brief Bash MCP server for shell command execution.
 *
 * Single tool: execute. Runs `/bin/sh -c` in the server's working
 * directory as the leader of its OWN process group, captures stdout and
 * stderr together, and enforces `timeout()` on the wall clock: on expiry
 * the whole group is SIGKILLed (background children included), the shell
 * is reaped, and the model gets a typed `timeout` error naming the limit
 * and the elapsed time, with whatever the command printed so far. When
 * the command finishes normally, anything it left running in its group
 * is killed too — a tool call does not leave processes behind. (A command
 * that deliberately leaves its group, e.g. via `setsid`, escapes this.)
 *
 * @par What it does NOT do
 * It does not filter commands. Nothing here inspects the command text:
 * whether a command runs at all is decided before execute() is reached,
 * by the permission layer (`permissions.allow` / `permissions.deny`
 * patterns, keyed by `get_permission_pattern()`) and the tool-approval
 * gate. Until v2.13.0 this comment claimed the server "blocks dangerous
 * commands" and "enforces timeout"; neither was true — there was never a
 * denylist, and the timeout was stored and never read.
 *
 * @version 2.13.0
 */
class BashServer : public MCPServerBase {
public:
    /**
     * @brief Construct with working directory and data dir.
     * @param working_dir Default working directory for commands.
     * @param data_dir Path to bundled data directory.
     * @param timeout Per-command wall-clock limit in seconds (enforced
     *        since v2.13.0; `mcp.bash.timeout_seconds`, default 30).
     * @version 2.13.0
     */
    BashServer(const std::filesystem::path& working_dir,
               const std::string& data_dir,
               int timeout = 30);

    ~BashServer() override;

    /**
     * @brief Permission pattern: "execute:{base_cmd} *".
     * @param tool_name Tool name.
     * @param args_json Arguments JSON.
     * @return Permission pattern with base command.
     * @version 1.8.5
     */
    std::string get_permission_pattern(
        const std::string& tool_name,
        const std::string& args_json) const override;

    /**
     * @brief Set working directory.
     * @param path New working directory.
     * @return true on success.
     * @version 1.8.5
     */
    bool set_working_dir(const std::string& path) override;

    /**
     * @brief Get the working directory.
     * @return Working directory path.
     * @version 1.8.5
     */
    const std::filesystem::path& working_dir() const;

    /**
     * @brief Get the enforced per-command timeout.
     * @return Timeout in seconds.
     * @version 2.13.0
     */
    int timeout() const;

private:
    std::filesystem::path working_dir_; ///< Default cwd
    int timeout_;                        ///< Enforced command timeout (s), immutable
    std::unique_ptr<ExecuteTool> execute_tool_;
};

} // namespace entropic
