// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file web.h
 * @brief Web MCP server — web_fetch + web_search.
 * @version 1.8.5
 */

#pragma once

#include <entropic/mcp/server_base.h>

#include <memory>
#include <string>

namespace entropic {

class WebFetchTool;
class WebSearchTool;

/**
 * @brief Web MCP server for web fetch and search.
 * @version 1.8.5
 */
class WebServer : public MCPServerBase {
public:
    /**
     * @brief Construct with data dir.
     * @param data_dir Path to bundled data directory.
     * @version 1.8.5
     */
    explicit WebServer(const std::string& data_dir);

    ~WebServer() override;

private:
    std::unique_ptr<WebFetchTool> web_fetch_;
    std::unique_ptr<WebSearchTool> web_search_;
};

} // namespace entropic
