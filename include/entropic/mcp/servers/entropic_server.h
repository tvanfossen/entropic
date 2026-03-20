/**
 * @file entropic_server.h
 * @brief Entropic MCP server — todo/delegate/pipeline/complete/phase_change/prune.
 *
 * All tools emit directives. delegate and pipeline skip duplicate check.
 *
 * @version 1.8.5
 */

#pragma once

#include <entropic/mcp/server_base.h>

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

/**
 * @brief Entropic MCP server for engine-level tools.
 *
 * All tools return ServerResponse with typed directives.
 * delegate and pipeline skip duplicate check.
 * Single-tier configs skip delegate/pipeline registration.
 *
 * @version 1.8.5
 */
class EntropicServer : public MCPServerBase {
public:
    /**
     * @brief Construct with tier names and data dir.
     * @param tier_names Available tier names for delegate/pipeline schemas.
     * @param data_dir Path to bundled data directory.
     * @version 1.8.5
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

private:
    std::unique_ptr<TodoTool> todo_;
    std::unique_ptr<DelegateTool> delegate_;
    std::unique_ptr<PipelineTool> pipeline_;
    std::unique_ptr<CompleteTool> complete_;
    std::unique_ptr<PhaseChangeTool> phase_change_;
    std::unique_ptr<PruneContextTool> prune_context_;
};

} // namespace entropic
