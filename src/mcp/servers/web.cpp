// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file web.cpp
 * @brief WebServer implementation — web_fetch + web_search placeholders.
 *
 * HTTP client dependency (cpp-httplib) is deferred. Tools load and register
 * so the server is structurally complete; execute() returns placeholder text.
 *
 * @version 1.8.5
 */

#include <entropic/mcp/servers/web.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

static auto logger = entropic::log::get("mcp.web");

namespace entropic {

// ── WebFetchTool ────────────────────────────────────────────────

/**
 * @brief Tool for fetching web page content by URL.
 * @internal
 * @version 1.8.5
 */
class WebFetchTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from web/web_fetch.json.
     * @internal
     * @version 1.8.5
     */
    explicit WebFetchTool(ToolDefinition def)
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
     * @brief Fetch web page content (placeholder).
     * @param args_json JSON with "url" and optional "max_length".
     * @return ServerResponse with placeholder text.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Parse fetch args and return placeholder.
 * @param args_json JSON with "url" and optional "max_length".
 * @return ServerResponse with placeholder result.
 * @internal
 * @version 1.8.5
 */
ServerResponse WebFetchTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string url = args.at("url").get<std::string>();

    static constexpr int default_max = 20000;
    static constexpr int cap_max = 50000;
    int max_length = args.value("max_length", default_max);
    max_length = std::min(max_length, cap_max);

    logger->info("[web_fetch] url='{}' max_length={}", url, max_length);

    return {
        "Web fetch not yet connected (requires cpp-httplib integration)",
        {}
    };
}

// ── WebSearchTool ───────────────────────────────────────────────

/**
 * @brief Tool for web search queries.
 * @internal
 * @version 1.8.5
 */
class WebSearchTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from web/web_search.json.
     * @internal
     * @version 1.8.5
     */
    explicit WebSearchTool(ToolDefinition def)
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
     * @brief Execute web search (placeholder).
     * @param args_json JSON with "query" and optional "max_results".
     * @return ServerResponse with placeholder text.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Parse search args and return placeholder.
 * @param args_json JSON with "query" and optional "max_results".
 * @return ServerResponse with placeholder result.
 * @internal
 * @version 1.8.5
 */
ServerResponse WebSearchTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string query = args.at("query").get<std::string>();

    static constexpr int default_max = 5;
    static constexpr int cap_max = 10;
    int max_results = args.value("max_results", default_max);
    max_results = std::min(max_results, cap_max);

    logger->info("[web_search] query='{}' max_results={}",
                 query, max_results);

    return {
        "Web search not yet connected (requires cpp-httplib integration)",
        {}
    };
}

// ── WebServer ───────────────────────────────────────────────────

/**
 * @brief Construct with data dir, register both tools.
 * @param data_dir Path to bundled data directory.
 * @internal
 * @version 1.8.5
 */
WebServer::WebServer(const std::string& data_dir)
    : MCPServerBase("web") {

    std::string tools_dir = data_dir + "/tools";

    auto fetch_def = load_tool_definition(
        "web_fetch", "web", tools_dir);
    web_fetch_ = std::make_unique<WebFetchTool>(
        std::move(fetch_def));

    auto search_def = load_tool_definition(
        "web_search", "web", tools_dir);
    web_search_ = std::make_unique<WebSearchTool>(
        std::move(search_def));

    register_tool(web_fetch_.get());
    register_tool(web_search_.get());

    logger->info("WebServer initialized with 2 tools");
}

/**
 * @brief Destructor.
 * @internal
 * @version 1.8.5
 */
WebServer::~WebServer() = default;

} // namespace entropic
