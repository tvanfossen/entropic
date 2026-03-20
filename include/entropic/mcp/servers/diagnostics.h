/**
 * @file diagnostics.h
 * @brief Diagnostics MCP server — LSP client for code diagnostics.
 * @version 1.8.5
 */

#pragma once

#include <entropic/mcp/server_base.h>

#include <filesystem>
#include <memory>
#include <string>

namespace entropic {

class DiagnosticsTool;
class CheckErrorsTool;

/**
 * @brief Diagnostics MCP server with LSP client.
 * @version 1.8.5
 */
class DiagnosticsServer : public MCPServerBase {
public:
    /**
     * @brief Construct with root directory and data dir.
     * @param root_dir Project root directory.
     * @param data_dir Path to bundled data directory.
     * @version 1.8.5
     */
    DiagnosticsServer(const std::filesystem::path& root_dir,
                      const std::string& data_dir);

    ~DiagnosticsServer() override;

    /**
     * @brief Get the root directory.
     * @return Root directory path.
     * @version 1.8.5
     */
    const std::filesystem::path& root_dir() const;

private:
    std::filesystem::path root_dir_; ///< Project root

    std::unique_ptr<DiagnosticsTool> diagnostics_;
    std::unique_ptr<CheckErrorsTool> check_errors_;
};

} // namespace entropic
