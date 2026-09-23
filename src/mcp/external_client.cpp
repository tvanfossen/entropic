// SPDX-License-Identifier: Apache-2.0
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
 * @dg_internal
 * @version 1.8.7
 */
ExternalMCPClient::ExternalMCPClient(
    std::string name,
    std::unique_ptr<Transport> transport)
    : name_(std::move(name)),
      transport_(std::move(transport)) {}

/**
 * @brief Connect: open transport, initialize, query tools.
 *
 * The MCP handshake in order — transport open, initialize, tools/list.
 *
 * @return true when the transport opened and initialize succeeded;
 *         false otherwise. A failed tools/list is NOT fatal: the client
 *         stays connected with zero tools and a warning, so a server
 *         that is up but tool-less does not read as down.
 * @req REQ-MCP-025
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
 * @dg_internal
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
 * @return The cached tool descriptors with names already
 *         `<server>.<tool>`-prefixed, so ServerManager can concatenate
 *         them without re-prefixing; "[]" when disconnected.
 * @req REQ-MCP-025
 * @req REQ-MCP-007
 * @version 1.8.7
 */
std::string ExternalMCPClient::list_tools() const {
    std::lock_guard<std::mutex> lock(tools_mutex_);
    return cached_tools_json_;
}

/**
 * @brief The right failure envelope for an empty transport response.
 *
 * gh#150: "timed out or transport error" covered four distinct conditions,
 * and the one that returns in 0 ms — a transport suppressed by an
 * interrupt — read exactly like a timeout that took no time. Separating it
 * is what makes the 0 ms case self-explaining instead of a contradiction
 * to chase; the reporting consumer lost real time to it.
 *
 * Extracted rather than inlined into execute(): the extra branch put that
 * function at 4 returns against a limit of 3, and the gate is not
 * negotiable.
 *
 * @param tool_name Local tool name (without server prefix).
 * @return An is_error envelope naming the actual condition.
 * @req REQ-MCP-025
 * @version 2.12.1
 */
std::string ExternalMCPClient::empty_response_envelope(
    const std::string& tool_name) const {
    if (transport_->is_interrupted()) {
        return build_response(
            "Error: tool '" + name_ + "." + tool_name +
            "' was not attempted — the transport is interrupted. "
            "The interrupt is released at the start of the next run.",
            true);
    }
    return build_response(
        "Error: tool '" + name_ + "." + tool_name +
        "' timed out or the transport failed.", true);
}

/**
 * @brief Execute a tool call via the external server.
 * @param tool_name Local name (without server prefix).
 * @param args_json JSON arguments; unparseable arguments degrade to an
 *                  empty object rather than throwing.
 * @return A ServerResponse JSON envelope whose directives array is
 *         ALWAYS empty — see build_response. A disconnected transport
 *         or an empty (timed-out) response yields an is_error envelope
 *         instead of a hang.
 * @req REQ-MCP-025
 * @req REQ-MCP-002
 * @req REQ-MCP-026
 * @version 2.13.0
 */
std::string ExternalMCPClient::execute(
    const std::string& tool_name,
    const std::string& args_json) {

    if (!transport_->is_connected()) {
        // gh#150: the leading "Error:" is load-bearing, not decoration.
        // classify_tool_result routes on the TEXT via looks_like_tool_error,
        // which matches a leading "error"/"[error]"/JSON-error shape. This
        // message used to start with "Server '...'", so a failed call was
        // logged status=ok and counted as a success — the reporting
        // consumer watched every external call fail while the logs said
        // everything was fine.
        return build_response(
            "Error: server '" + name_ + "' is disconnected. "
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

    int request_id = 0;
    auto request = build_request_id(
        "tools/call", params.dump(), request_id);
    auto response = transport_->send_request(
        request, DEFAULT_TIMEOUT_MS);

    auto problem = response_problem(tool_name, response, request_id);
    if (!problem.empty()) {
        return problem;
    }

    auto result_text = extract_tool_result(response);
    return build_response(result_text);
}

/**
 * @brief The failure envelope a response warrants, or empty if it is fine.
 *
 * gh#158. `execute()` gained a second rejection reason (the JSON-RPC id
 * pairing) and that would have put it at FOUR returns against a limit of
 * three. Both rejections are "this response cannot be used", so they belong
 * together rather than as two more early exits.
 *
 * @param tool_name Local tool name (without server prefix).
 * @param response Raw response line from the transport.
 * @param expected_id The id the request carried.
 * @return An is_error envelope, or an empty string when the response is
 *         usable (`build_response` never produces an empty string, so empty
 *         is an unambiguous "no problem").
 * @req REQ-MCP-025
 * @req REQ-MCP-026
 * @version 2.13.0
 */
std::string ExternalMCPClient::response_problem(
    const std::string& tool_name,
    const std::string& response,
    int expected_id) const {
    if (response.empty()) {
        return empty_response_envelope(tool_name);
    }
    if (!response_matches(response, expected_id)) {
        return mismatched_response_envelope(tool_name, expected_id);
    }
    return {};
}

/**
 * @brief The envelope for a response that answered a different request.
 *
 * gh#158. Sibling of `empty_response_envelope`, reached through
 * `response_problem`.
 *
 * Reported as an error rather than returned as a result, because the payload
 * is REAL content that belongs to someone else's tool call — the one failure
 * mode a caller could not detect for itself. The leading "Error:" is
 * load-bearing: `classify_tool_result` routes on the text.
 *
 * @param tool_name Local tool name (without server prefix).
 * @param expected_id The id this call sent.
 * @return An is_error envelope naming the desynchronization.
 * @req REQ-MCP-025
 * @req REQ-MCP-026
 * @version 2.13.0
 */
std::string ExternalMCPClient::mismatched_response_envelope(
    const std::string& tool_name, int expected_id) const {
    logger->error("Server '{}' answered request {} with a response carrying "
                  "a different id; discarding it rather than returning "
                  "another request's result for tool '{}'",
                  name_, expected_id, tool_name);
    return build_response(
        "Error: tool '" + name_ + "." + tool_name +
        "' got a response that did not answer it (JSON-RPC id mismatch). "
        "The transport had a stale reply queued from an abandoned request; "
        "it has been discarded. Retry the call.", true);
}

/**
 * @brief Names in `a` not in `b` (sorted set difference).
 * @param a Superset candidate.
 * @param b Set to subtract.
 * @return a \ b as a vector.
 * @utility
 * @version 2.3.7
 */
static std::vector<std::string> names_diff(
    const std::set<std::string>& a, const std::set<std::string>& b) {
    std::vector<std::string> out;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(out));
    return out;
}

/**
 * @brief Re-query tools/list and diff against cache.
 *
 * Called after a successful reconnect so the model's tool list tracks
 * a server that changed while it was down.
 *
 * @return A pair of (added, removed) fully-qualified tool names; both
 *         empty when the server's surface is unchanged.
 * @req REQ-MCP-025
 * @version 2.3.7
 */
std::pair<std::vector<std::string>, std::vector<std::string>>
ExternalMCPClient::refresh_tools() {
    auto snapshot = [this] {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        return std::set<std::string>(cached_tool_names_.begin(),
                                     cached_tool_names_.end());
    };

    std::set<std::string> old_names = snapshot();
    query_tools();
    std::set<std::string> new_names = snapshot();

    auto added = names_diff(new_names, old_names);
    auto removed = names_diff(old_names, new_names);

    logger->info("Server '{}' tools refreshed: +{} -{}",
                 name_, added.size(), removed.size());
    return {added, removed};
}

/**
 * @brief Check connection state via transport.
 * @return true if connected.
 * @dg_internal
 * @version 1.8.7
 */
bool ExternalMCPClient::is_connected() const {
    return transport_->is_connected();
}

/**
 * @brief Build a JSON-RPC 2.0 request envelope.
 * @param method JSON-RPC method name.
 * @param params JSON-RPC params string.
 * @return A JSON-RPC 2.0 request carrying a monotonically increasing
 *         id; unparseable params degrade to an empty object rather than
 *         throwing.
 * @req REQ-MCP-025
 * @version 2.13.0
 */
std::string ExternalMCPClient::build_request(
    const std::string& method,
    const std::string& params) {
    int ignored = 0;
    return build_request_id(method, params, ignored);
}

/**
 * @brief Build a JSON-RPC request and report its id (gh#158).
 * @param method JSON-RPC method name.
 * @param params JSON-RPC params string.
 * @param[out] id The id stamped into the request.
 * @return A JSON-RPC 2.0 request carrying a monotonically increasing id;
 *         unparseable params degrade to an empty object rather than throwing.
 * @req REQ-MCP-025
 * @req REQ-MCP-026
 * @version 2.13.0
 */
std::string ExternalMCPClient::build_request_id(
    const std::string& method,
    const std::string& params,
    int& id) {

    id = next_id_.fetch_add(1, std::memory_order_relaxed);
    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"] = id;
    req["method"] = method;
    try {
        req["params"] = nlohmann::json::parse(params);
    } catch (...) {
        req["params"] = nlohmann::json::object();
    }
    return req.dump();
}

/**
 * @brief Whether a response answers the request that asked — see header.
 * @param response_json Raw JSON-RPC response line.
 * @param expected_id The id of the request that was sent.
 * @return true when the response carries `expected_id`; false on a
 *         mismatch, a missing id, or an unparseable line.
 * @req REQ-MCP-025
 * @req REQ-MCP-026
 * @version 2.13.0
 */
bool ExternalMCPClient::response_matches(
    const std::string& response_json, int expected_id) {
    try {
        auto j = nlohmann::json::parse(response_json);
        // A string id is accepted when it spells the same number. We always
        // SEND an integer and JSON-RPC 2.0 requires the response to echo the
        // same value, but stringifying it is the one deviation common enough
        // in the wild to be worth tolerating — it is unambiguous, so
        // rejecting it would break working servers for no safety gain.
        if (j.value("id", nlohmann::json()).is_string()) {
            return j["id"].get<std::string>()
                == std::to_string(expected_id);
        }
        return j.contains("id") && j["id"].is_number_integer()
            && j["id"].get<int>() == expected_id;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
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
 * @return true when the server answered within the init timeout and the
 *         response carried no JSON-RPC error; false on timeout, empty
 *         response, or an error object.
 * @req REQ-MCP-025
 * @req REQ-MCP-026
 * @version 2.13.0
 */
bool ExternalMCPClient::send_initialize() {
    nlohmann::json params;
    params["protocolVersion"] = "2024-11-05";
    params["capabilities"] = nlohmann::json::object();
    params["clientInfo"]["name"] = "entropic";
    params["clientInfo"]["version"] = "1.8.7";

    int request_id = 0;
    auto request = build_request_id(
        "initialize", params.dump(), request_id);
    auto response = transport_->send_request(
        request, INIT_TIMEOUT_MS);

    if (response.empty() || !response_matches(response, request_id)) {
        return false;
    }
    return validate_init_response(response);
}

/**
 * @brief Query tools/list and update cache with prefixed names.
 *
 * Prefixing happens once, here, so the cached descriptors are already
 * in `<server>.<tool>` routing form.
 *
 * @return true when the tool list was fetched and cached; false on
 *         timeout, empty response, or an unparseable/misshaped result.
 * @req REQ-MCP-025
 * @req REQ-MCP-007
 * @req REQ-MCP-026
 * @version 2.13.0
 */
bool ExternalMCPClient::query_tools() {
    int request_id = 0;
    auto request = build_request_id("tools/list", "{}", request_id);
    auto response = transport_->send_request(
        request, INIT_TIMEOUT_MS);

    if (response.empty() || !response_matches(response, request_id)) {
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
 *
 * The single construction point for every external response, which is
 * what makes the guarantee absolute: whatever an external server put in
 * its own `directives` array is discarded here, so it can never inject
 * delegate, stop_processing, phase_change or any other engine-level
 * directive (CWE-94).
 *
 * @param result_text Result text.
 * @param is_error true if error.
 * @return A ServerResponse envelope with the text in `result` and an
 *         ALWAYS-empty `directives` array, plus is_error when set.
 * @req REQ-MCP-025
 * @req REQ-MCP-002
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
