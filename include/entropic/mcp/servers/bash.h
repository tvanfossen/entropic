// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file bash.h
 * @brief Bash MCP server — shell command execution.
 * @version 1.8.5
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
 * Single tool: execute. Blocks dangerous commands, captures
 * stdout + stderr, enforces timeout.
 *
 * @version 1.8.5
 */
class BashServer : public MCPServerBase {
public:
    /**
     * @brief Construct with working directory and data dir.
     * @param working_dir Default working directory for commands.
     * @param data_dir Path to bundled data directory.
     * @param timeout Command timeout in seconds.
     * @version 1.8.5
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
     * @brief Get command timeout.
     * @return Timeout in seconds.
     * @version 1.8.5
     */
    int timeout() const;

private:
    std::filesystem::path working_dir_; ///< Default cwd
    int timeout_;                        ///< Command timeout (seconds)
    std::unique_ptr<ExecuteTool> execute_tool_;
};

} // namespace entropic
