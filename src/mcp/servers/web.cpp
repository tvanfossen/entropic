// SPDX-License-Identifier: Apache-2.0
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
 * @dg_internal
 * @version 1.8.5
 */
class WebFetchTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from web/web_fetch.json.
     * @dg_internal
     * @version 1.8.5
     */
    explicit WebFetchTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool has no side effects.
     * @req REQ-MCP-011
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Fetch web page content (placeholder).
     * @param args_json JSON with "url" and optional "max_length".
     * @return ServerResponse with placeholder text.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Parse fetch args and return the declared not_implemented error.
 * @param args_json JSON with "url" and optional "max_length".
 * @return ServerResponse with `{"error":"not_implemented",...}`.
 * @dg_internal
 * @version 2.14.0
 */
ServerResponse WebFetchTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string url = args.at("url").get<std::string>();

    static constexpr int default_max = 20000;
    static constexpr int cap_max = 50000;
    int max_length = args.value("max_length", default_max);
    max_length = std::min(max_length, cap_max);

    logger->info("[web_fetch] url='{}' max_length={}", url, max_length);

    // v2.14.0 (gh#174/#203): a declared error, not placeholder prose a
    // model could mistake for a fetched page or a result list.
    return {
        tool_error_json("not_implemented", "Web fetch is not implemented in this build; "
                                   "the web server is a placeholder (mcp.enable_web)."),
        {}
    };
}

// ── WebSearchTool ───────────────────────────────────────────────

/**
 * @brief Tool for web search queries.
 * @dg_internal
 * @version 1.8.5
 */
class WebSearchTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from web/web_search.json.
     * @dg_internal
     * @version 1.8.5
     */
    explicit WebSearchTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool has no side effects.
     * @req REQ-MCP-011
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Execute web search (placeholder).
     * @param args_json JSON with "query" and optional "max_results".
     * @return ServerResponse with placeholder text.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Parse search args and return the declared not_implemented error.
 * @param args_json JSON with "query" and optional "max_results".
 * @return ServerResponse with `{"error":"not_implemented",...}`.
 * @dg_internal
 * @version 2.14.0
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

    // v2.14.0 (gh#174/#203): a declared error, not placeholder prose a
    // model could mistake for a fetched page or a result list.
    return {
        tool_error_json("not_implemented", "Web search is not implemented in this build; "
                                   "the web server is a placeholder (mcp.enable_web)."),
        {}
    };
}

// ── WebServer ───────────────────────────────────────────────────

/**
 * @brief Construct with data dir, register both tools.
 *
 * Another override-nothing MCPServerBase subclass: build the two tools,
 * register them, and inherit dispatch, envelope shape and the default
 * extension-point behaviour from the base.
 *
 * @param data_dir Path to bundled data directory.
 * @req REQ-MCP-001
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
 * @dg_internal
 * @version 1.8.5
 */
WebServer::~WebServer() = default;

} // namespace entropic
