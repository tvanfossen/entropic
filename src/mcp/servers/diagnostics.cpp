// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file diagnostics.cpp
 * @brief DiagnosticsServer implementation — LSP client stubs.
 *
 * The actual LSP client is a substantial subsystem deferred to v1.8.7.
 * These tools exist so the server loads and tests pass. Each returns
 * a placeholder message indicating the LSP backend is not yet wired.
 *
 * @version 1.8.5
 */

#include <entropic/mcp/servers/diagnostics.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <string>

static auto logger = entropic::log::get("mcp.diagnostics");

namespace entropic {

// ── DiagnosticsTool (stub) ──────────────────────────────────────

/**
 * @brief Stub tool for LSP diagnostics retrieval.
 *
 * Returns a placeholder until the LSP client lands in v1.8.7.
 *
 * @internal
 * @version 1.8.5
 */
class DiagnosticsTool : public ToolBase {
public:
    /**
     * @brief Construct with definition.
     * @param def Tool definition loaded from JSON.
     * @internal
     * @version 1.8.5
     */
    explicit DiagnosticsTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

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
     * @brief Return placeholder — LSP not yet connected.
     * @param args_json Unused.
     * @return ServerResponse with stub message.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        (void)args_json;
        logger->info("[diagnostics] stub invoked");
        return {"LSP diagnostics not yet connected (v1.8.7)", {}};
    }
};

// ── CheckErrorsTool (stub) ──────────────────────────────────────

/**
 * @brief Stub tool for LSP error checking.
 *
 * Returns a placeholder until the LSP client lands in v1.8.7.
 *
 * @internal
 * @version 1.8.5
 */
class CheckErrorsTool : public ToolBase {
public:
    /**
     * @brief Construct with definition.
     * @param def Tool definition loaded from JSON.
     * @internal
     * @version 1.8.5
     */
    explicit CheckErrorsTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

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
     * @brief Return placeholder — LSP not yet connected.
     * @param args_json Unused.
     * @return ServerResponse with stub message.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        (void)args_json;
        logger->info("[check_errors] stub invoked");
        return {"LSP check_errors not yet connected (v1.8.7)", {}};
    }
};

// ── DiagnosticsServer ───────────────────────────────────────────

/**
 * @brief Construct with root directory and data dir.
 * @param root_dir Project root directory.
 * @param data_dir Path to bundled data directory.
 * @internal
 * @version 1.8.5
 */
DiagnosticsServer::DiagnosticsServer(
    const std::filesystem::path& root_dir,
    const std::string& data_dir)
    : MCPServerBase("diagnostics")
    , root_dir_(root_dir) {

    std::string tools_dir = data_dir + "/tools";

    diagnostics_ = std::make_unique<DiagnosticsTool>(
        load_tool_definition(
            "diagnostics", "diagnostics", tools_dir));

    check_errors_ = std::make_unique<CheckErrorsTool>(
        load_tool_definition(
            "check_errors", "diagnostics", tools_dir));

    register_tool(diagnostics_.get());
    register_tool(check_errors_.get());

    logger->info("DiagnosticsServer initialized: root='{}'",
                 root_dir_.string());
}

/**
 * @brief Destructor.
 * @internal
 * @version 1.8.5
 */
DiagnosticsServer::~DiagnosticsServer() = default;

/**
 * @brief Get the root directory.
 * @return Root directory path.
 * @internal
 * @version 1.8.5
 */
const std::filesystem::path& DiagnosticsServer::root_dir() const {
    return root_dir_;
}

} // namespace entropic
