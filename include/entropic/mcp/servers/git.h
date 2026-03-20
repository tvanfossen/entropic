/**
 * @file git.h
 * @brief Git MCP server — version control operations.
 * @version 1.8.5
 */

#pragma once

#include <entropic/mcp/server_base.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace entropic {

class GitStatusTool;
class GitDiffTool;
class GitLogTool;
class GitCommitTool;
class GitBranchTool;
class GitCheckoutTool;
class GitAddTool;
class GitResetTool;

/**
 * @brief Git MCP server for version control operations.
 * @version 1.8.5
 */
class GitServer : public MCPServerBase {
public:
    /**
     * @brief Construct with repo directory and data dir.
     * @param repo_dir Repository root directory.
     * @param data_dir Path to bundled data directory.
     * @version 1.8.5
     */
    GitServer(const std::filesystem::path& repo_dir,
              const std::string& data_dir);

    ~GitServer() override;

    /**
     * @brief Set working directory (repo root).
     * @param path New repo directory.
     * @return true on success.
     * @version 1.8.5
     */
    bool set_working_dir(const std::string& path) override;

    /**
     * @brief Get repo directory.
     * @return Repo root path.
     * @version 1.8.5
     */
    const std::filesystem::path& repo_dir() const;

private:
    std::filesystem::path repo_dir_; ///< Repository root

    std::unique_ptr<GitStatusTool> status_;
    std::unique_ptr<GitDiffTool> diff_;
    std::unique_ptr<GitLogTool> log_;
    std::unique_ptr<GitCommitTool> commit_;
    std::unique_ptr<GitBranchTool> branch_;
    std::unique_ptr<GitCheckoutTool> checkout_;
    std::unique_ptr<GitAddTool> add_;
    std::unique_ptr<GitResetTool> reset_;
};

} // namespace entropic
