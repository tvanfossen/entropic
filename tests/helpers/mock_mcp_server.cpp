// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file mock_mcp_server.cpp
 * @brief Minimal mock MCP server for integration tests.
 *
 * Speaks JSON-RPC 2.0 over stdin/stdout (stdio transport).
 * Responds to initialize, tools/list, tools/call.
 * Used as a child process by StdioTransport in tests.
 *
 * Usage: mock_mcp_server [--tools TOOL1,TOOL2] [--fail-after N]
 *   --tools: Comma-separated tool names to advertise (default: "echo")
 *   --fail-after: Exit after N tool calls (simulates crash)
 *
 * @version 1.8.7
 */

#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @brief Parse comma-separated tool names.
 * @param csv Comma-separated string.
 * @return Vector of tool names.
 * @utility
 * @version 1.8.7
 */
static std::vector<std::string> parse_tools(const std::string& csv) {
    std::vector<std::string> result;
    std::istringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

/**
 * @brief Build tools/list response.
 * @param id Request ID.
 * @param tool_names Tool names to advertise.
 * @return JSON-RPC response.
 * @utility
 * @version 1.8.7
 */
static json build_tools_list(int id,
                             const std::vector<std::string>& tool_names) {
    auto tools = json::array();
    for (const auto& name : tool_names) {
        json tool;
        tool["name"] = name;
        tool["description"] = "Mock tool: " + name;
        tool["inputSchema"] = {
            {"type", "object"},
            {"properties", {{"text", {{"type", "string"}}}}},
        };
        tools.push_back(tool);
    }

    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"]["tools"] = tools;
    return resp;
}

/**
 * @brief Build tools/call response (echo behavior).
 * @param id Request ID.
 * @param params Call params.
 * @return JSON-RPC response.
 * @utility
 * @version 1.8.7
 */
static json build_tools_call(int id, const json& params) {
    std::string text = "echo: ";
    if (params.contains("arguments") &&
        params["arguments"].contains("text")) {
        text += params["arguments"]["text"].get<std::string>();
    } else {
        text += "(no text)";
    }

    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"]["content"] = json::array({
        {{"type", "text"}, {"text", text}}
    });
    return resp;
}

/**
 * @brief Build initialize response.
 * @param id Request ID.
 * @return JSON-RPC response.
 * @utility
 * @version 1.8.7
 */
static json build_initialize(int id) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"]["protocolVersion"] = "2024-11-05";
    resp["result"]["serverInfo"]["name"] = "mock-mcp";
    resp["result"]["serverInfo"]["version"] = "1.0.0";
    resp["result"]["capabilities"]["tools"] = json::object();
    return resp;
}

/**
 * @brief Build error response for unknown methods.
 * @param id Request ID.
 * @return JSON-RPC error response.
 * @utility
 * @version 1.8.8
 */
static json build_method_not_found(int id) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["error"]["code"] = -32601;
    resp["error"]["message"] = "Method not found";
    return resp;
}

/**
 * @brief Dispatch a JSON-RPC request to the appropriate handler.
 * @param request Parsed JSON-RPC request.
 * @param tool_names Available tool names.
 * @param call_count Running tool call count (mutated).
 * @param fail_after Crash threshold (-1 = disabled).
 * @return Response JSON, or nullopt to signal simulated crash.
 * @utility
 * @version 1.8.8
 */
/**
 * @brief Handle tools/call with optional crash simulation.
 * @param id Request ID.
 * @param params Call params.
 * @param call_count Running count (mutated).
 * @param fail_after Crash threshold.
 * @return Response, or nullopt to signal crash.
 * @utility
 * @version 1.8.7
 */
static std::optional<json> handle_tools_call(
    int id, const json& params,
    int& call_count, int fail_after) {

    ++call_count;
    if (fail_after > 0 && call_count >= fail_after) {
        return std::nullopt;
    }
    return build_tools_call(id, params);
}

/**
 * @brief Dispatch a JSON-RPC request to the appropriate handler.
 * @param request Parsed JSON-RPC request.
 * @param tool_names Available tool names.
 * @param call_count Running tool call count (mutated).
 * @param fail_after Crash threshold (-1 = disabled).
 * @return Response JSON, or nullopt to signal simulated crash.
 * @utility
 * @version 1.8.7
 */
static std::optional<json> dispatch_request(
    const json& request,
    const std::vector<std::string>& tool_names,
    int& call_count,
    int fail_after) {

    auto method = request.value("method", "");
    int id = request.value("id", 0);
    auto params = request.value("params", json::object());

    if (method == "tools/call") {
        return handle_tools_call(id, params, call_count, fail_after);
    }

    std::optional<json> resp;
    if (method == "initialize") {
        resp = build_initialize(id);
    } else if (method == "tools/list") {
        resp = build_tools_list(id, tool_names);
    } else {
        resp = build_method_not_found(id);
    }
    return resp;
}

/**
 * @brief Parse CLI arguments into tool names and fail-after.
 * @param argc Argument count.
 * @param argv Arguments.
 * @param[out] tool_names Populated tool list.
 * @param[out] fail_after Crash threshold.
 * @utility
 * @version 1.8.8
 */
static void parse_args(
    int argc, char* argv[],
    std::vector<std::string>& tool_names,
    int& fail_after) {

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--tools" && i + 1 < argc) {
            tool_names = parse_tools(argv[++i]);
        } else if (arg == "--fail-after" && i + 1 < argc) {
            fail_after = std::stoi(argv[++i]);
        }
    }
}

/**
 * @brief Main loop: read JSON-RPC from stdin, respond on stdout.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return Exit code.
 * @internal
 * @version 1.8.7
 */
int main(int argc, char* argv[]) {
    std::vector<std::string> tool_names = {"echo"};
    int fail_after = -1;
    int call_count = 0;
    parse_args(argc, argv, tool_names, fail_after);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        json request;
        try {
            request = json::parse(line);
        } catch (...) {
            continue;
        }

        auto response = dispatch_request(
            request, tool_names, call_count, fail_after);
        if (!response.has_value()) {
            return 1;
        }
        std::cout << response->dump() << "\n" << std::flush;
    }

    return 0;
}
