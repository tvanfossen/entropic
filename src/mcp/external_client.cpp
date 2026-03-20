/**
 * @file external_client.cpp
 * @brief ExternalMCPClient implementation.
 * @version 1.8.7
 */

#include <entropic/mcp/external_client.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

static auto logger = entropic::log::get("mcp.external_client");

namespace entropic {

/**
 * @brief Construct with name and transport.
 * @param name Server name.
 * @param transport Ownership transferred.
 * @internal
 * @version 1.8.7
 */
ExternalMCPClient::ExternalMCPClient(
    std::string name,
    std::unique_ptr<Transport> transport)
    : name_(std::move(name)),
      transport_(std::move(transport)) {}

/**
 * @brief Connect: open transport, initialize, query tools.
 * @return true on success.
 * @internal
 * @version 1.8.7
 */
bool ExternalMCPClient::connect() {
    if (!transport_->open()) {
        logger->error("Transport open failed for '{}'", name_);
        return false;
    }

    if (!send_initialize()) {
        logger->error("MCP initialize failed for '{}'", name_);
        transport_->close();
        return false;
    }

    if (!query_tools()) {
        logger->warn("tools/list failed for '{}' — "
                     "connected with 0 tools", name_);
    }

    logger->info("Connected to '{}': {} tools",
                 name_, cached_tool_names_.size());
    return true;
}

/**
 * @brief Disconnect: close transport, clear cache.
 * @internal
 * @version 1.8.7
 */
void ExternalMCPClient::disconnect() {
    transport_->close();
    std::lock_guard<std::mutex> lock(tools_mutex_);
    cached_tools_json_ = "[]";
    cached_tool_names_.clear();
    logger->info("Disconnected from '{}'", name_);
}

/**
 * @brief List tools as JSON array string (cached, prefixed).
 * @return JSON array string.
 * @internal
 * @version 1.8.7
 */
std::string ExternalMCPClient::list_tools() const {
    std::lock_guard<std::mutex> lock(tools_mutex_);
    return cached_tools_json_;
}

/**
 * @brief Execute a tool call via the external server.
 * @param tool_name Local name (without server prefix).
 * @param args_json JSON arguments.
 * @return ServerResponse JSON (directives always empty).
 * @internal
 * @version 1.8.7
 */
std::string ExternalMCPClient::execute(
    const std::string& tool_name,
    const std::string& args_json) {

    if (!transport_->is_connected()) {
        return build_response(
            "Server '" + name_ + "' is disconnected. "
            "Tool '" + name_ + "." + tool_name + "' unavailable.",
            true);
    }

    nlohmann::json params;
    params["name"] = tool_name;
    try {
        params["arguments"] = nlohmann::json::parse(args_json);
    } catch (...) {
        params["arguments"] = nlohmann::json::object();
    }

    auto request = build_request("tools/call", params.dump());
    auto response = transport_->send_request(
        request, DEFAULT_TIMEOUT_MS);

    if (response.empty()) {
        return build_response(
            "Tool '" + name_ + "." + tool_name +
            "' timed out or transport error.", true);
    }

    auto result_text = extract_tool_result(response);
    return build_response(result_text);
}

/**
 * @brief Re-query tools/list and diff against cache.
 * @return Pair of (added, removed) tool name vectors.
 * @internal
 * @version 1.8.7
 */
std::pair<std::vector<std::string>, std::vector<std::string>>
ExternalMCPClient::refresh_tools() {

    std::set<std::string> old_names;
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        old_names.insert(cached_tool_names_.begin(),
                         cached_tool_names_.end());
    }

    query_tools();

    std::set<std::string> new_names;
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        new_names.insert(cached_tool_names_.begin(),
                         cached_tool_names_.end());
    }

    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::set_difference(new_names.begin(), new_names.end(),
                        old_names.begin(), old_names.end(),
                        std::back_inserter(added));
    std::set_difference(old_names.begin(), old_names.end(),
                        new_names.begin(), new_names.end(),
                        std::back_inserter(removed));

    logger->info("Server '{}' tools refreshed: +{} -{}",
                 name_, added.size(), removed.size());
    return {added, removed};
}

/**
 * @brief Check connection state via transport.
 * @return true if connected.
 * @internal
 * @version 1.8.7
 */
bool ExternalMCPClient::is_connected() const {
    return transport_->is_connected();
}

/**
 * @brief Build a JSON-RPC 2.0 request envelope.
 * @param method JSON-RPC method name.
 * @param params JSON-RPC params string.
 * @return JSON-RPC request string.
 * @utility
 * @version 1.8.7
 */
std::string ExternalMCPClient::build_request(
    const std::string& method,
    const std::string& params) {

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"] = next_id_++;
    req["method"] = method;
    try {
        req["params"] = nlohmann::json::parse(params);
    } catch (...) {
        req["params"] = nlohmann::json::object();
    }
    return req.dump();
}

/**
 * @brief Validate an initialize response for errors.
 * @param response Raw JSON-RPC response string.
 * @return true if response is valid and error-free.
 * @utility
 * @version 1.8.8
 */
bool ExternalMCPClient::validate_init_response(
    const std::string& response) {

    try {
        auto j = nlohmann::json::parse(response);
        if (j.contains("error")) {
            logger->error("Initialize error from '{}': {}",
                          name_, j["error"].dump());
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * @brief Send MCP initialize handshake.
 * @return true on success.
 * @utility
 * @version 1.8.8
 */
bool ExternalMCPClient::send_initialize() {
    nlohmann::json params;
    params["protocolVersion"] = "2024-11-05";
    params["capabilities"] = nlohmann::json::object();
    params["clientInfo"]["name"] = "entropic";
    params["clientInfo"]["version"] = "1.8.7";

    auto request = build_request("initialize", params.dump());
    auto response = transport_->send_request(
        request, INIT_TIMEOUT_MS);

    if (response.empty()) {
        return false;
    }
    return validate_init_response(response);
}

/**
 * @brief Query tools/list and update cache with prefixed names.
 * @return true on success.
 * @utility
 * @version 1.8.7
 */
bool ExternalMCPClient::query_tools() {
    auto request = build_request("tools/list");
    auto response = transport_->send_request(
        request, INIT_TIMEOUT_MS);

    if (response.empty()) {
        return false;
    }

    try {
        auto j = nlohmann::json::parse(response);
        auto tools = j.at("result").at("tools");

        // Prefix tool names with server name
        std::vector<std::string> names;
        for (auto& tool : tools) {
            std::string orig = tool["name"].get<std::string>();
            tool["name"] = name_ + "." + orig;
            names.push_back(tool["name"].get<std::string>());
        }

        std::lock_guard<std::mutex> lock(tools_mutex_);
        cached_tools_json_ = tools.dump();
        cached_tool_names_ = std::move(names);
        return true;
    } catch (const nlohmann::json::exception& e) {
        logger->error("Failed to parse tools/list from '{}': {}",
                      name_, e.what());
        return false;
    }
}

/**
 * @brief Extract text content from tools/call JSON-RPC response.
 * @param response_json Raw JSON-RPC response.
 * @return Extracted text, or error message.
 * @utility
 * @version 1.8.7
 */
std::string ExternalMCPClient::extract_tool_result(
    const std::string& response_json) {

    try {
        auto j = nlohmann::json::parse(response_json);
        if (j.contains("error")) {
            return "Error: " + j["error"]["message"]
                .get<std::string>();
        }

        auto& content = j.at("result").at("content");
        std::string text;
        for (const auto& item : content) {
            if (item.value("type", "") == "text") {
                text += item.at("text").get<std::string>();
            }
        }
        return text;
    } catch (const nlohmann::json::exception& e) {
        return "Error parsing response: " + std::string(e.what());
    }
}

/**
 * @brief Build ServerResponse JSON with empty directives (security).
 * @param result_text Result text.
 * @param is_error true if error.
 * @return ServerResponse JSON envelope.
 * @utility
 * @version 1.8.7
 */
std::string ExternalMCPClient::build_response(
    const std::string& result_text,
    bool is_error) {

    nlohmann::json resp;
    resp["result"] = result_text;
    // SECURITY: External servers CANNOT inject directives.
    // Directives array is always empty for external tool results.
    resp["directives"] = nlohmann::json::array();
    if (is_error) {
        resp["is_error"] = true;
    }
    return resp.dump();
}

} // namespace entropic
