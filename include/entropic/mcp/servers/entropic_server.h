// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file entropic_server.h
 * @brief Entropic MCP server — engine-level tools including introspection.
 *
 * All tools emit directives. delegate and pipeline skip duplicate check.
 * Diagnose and inspect are read-only (no directives).
 *
 * @version 1.9.12
 */

#pragma once

#include <entropic/interfaces/i_mcp_server.h>
#include <entropic/mcp/server_base.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace entropic {

class TodoTool;
class DelegateTool;
class PipelineTool;
class CompleteTool;
class PhaseChangeTool;
class PruneContextTool;
class DiagnoseTool;
class InspectTool;

/**
 * @brief Entropic MCP server for engine-level tools.
 *
 * All tools return ServerResponse with typed directives.
 * delegate and pipeline skip duplicate check.
 * Single-tier configs skip delegate/pipeline registration.
 * Diagnose and inspect provide read-only engine introspection.
 *
 * @version 1.9.12
 */
class EntropicServer : public MCPServerBase {
public:
    /**
     * @brief Construct with tier names and data dir.
     * @param tier_names Available tier names for delegate/pipeline schemas.
     * @param data_dir Path to bundled data directory.
     * @version 1.9.12
     */
    EntropicServer(const std::vector<std::string>& tier_names,
                   const std::string& data_dir);

    ~EntropicServer() override;

    /**
     * @brief delegate and pipeline skip duplicate check.
     * @param tool_name Tool name.
     * @return true for delegate and pipeline.
     * @version 1.8.5
     */
    bool skip_duplicate_check(const std::string& tool_name) const override;

    /**
     * @brief Set the engine state provider for introspection tools.
     * @param provider Callback struct with engine state accessors.
     * @version 1.9.12
     */
    void set_state_provider(const entropic_state_provider_t& provider);

private:
    std::unique_ptr<TodoTool> todo_;
    std::unique_ptr<DelegateTool> delegate_;
    std::unique_ptr<PipelineTool> pipeline_;
    std::unique_ptr<CompleteTool> complete_;
    std::unique_ptr<PhaseChangeTool> phase_change_;
    std::unique_ptr<PruneContextTool> prune_context_;
    std::unique_ptr<DiagnoseTool> diagnose_;     ///< v1.9.12 introspection
    std::unique_ptr<InspectTool> inspect_;        ///< v1.9.12 introspection
    entropic_state_provider_t state_provider_{};  ///< Stored copy for lifetime

    /** @brief Register core tools (todo, complete, phase_change, prune).
     * @internal @version 1.9.12 */
    int register_core_tools(const std::string& tools_dir);

    /** @brief Register delegation tools if multi-tier.
     * @internal @version 1.9.12 */
    int register_delegation_tools(
        const std::string& tools_dir,
        const std::vector<std::string>& tier_names);

    /** @brief Register introspection tools (diagnose, inspect).
     * @internal @version 1.9.12 */
    int register_introspection_tools(const std::string& tools_dir);
};

} // namespace entropic
