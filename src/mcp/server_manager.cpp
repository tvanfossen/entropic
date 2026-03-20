/**
 * @file server_manager.cpp
 * @brief ServerManager implementation.
 * @version 1.8.5
 */

#include <entropic/mcp/server_manager.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

static auto logger = entropic::log::get("mcp.server_manager");

namespace entropic {

/**
 * @brief Construct with permission config and project directory.
 * @param permissions Permission configuration.
 * @param project_dir Project root directory.
 * @internal
 * @version 1.8.5
 */
ServerManager::ServerManager(
    const PermissionsConfig& permissions,
    const std::filesystem::path& project_dir)
    : permissions_(permissions.allow, permissions.deny),
      project_dir_(project_dir) {}

/**
 * @brief Register a built-in server.
 * @param server Server instance (ownership transferred).
 * @internal
 * @version 1.8.5
 */
void ServerManager::register_server(
    std::unique_ptr<MCPServerBase> server) {
    auto name = server->name();
    if (servers_.count(name) > 0) {
        logger->warn("Server '{}' already registered — replacing",
                     name);
    }
    servers_[name] = std::move(server);
    logger->info("Registered server: {}", name);
}

/**
 * @brief Initialize all registered servers.
 * @internal
 * @version 1.8.5
 */
void ServerManager::initialize() {
    logger->info("Initializing {} MCP servers", servers_.size());
    for (auto& [name, server] : servers_) {
        logger->info("Server '{}' ready", name);
    }
}

/**
 * @brief List all tools from all connected servers.
 * @return JSON array string.
 * @internal
 * @version 1.8.5
 */
std::string ServerManager::list_tools() const {
    auto all = nlohmann::json::array();
    for (const auto& [name, server] : servers_) {
        auto tools_json = server->list_tools();
        auto tools = nlohmann::json::parse(tools_json);
        for (auto& tool : tools) {
            // Prefix tool names with server name
            std::string orig_name = tool["name"];
            tool["name"] = name + "." + orig_name;
            all.push_back(std::move(tool));
        }
    }
    return all.dump();
}

/**
 * @brief Execute a tool call via the appropriate server.
 * @param tool_name Fully-qualified name.
 * @param args_json JSON arguments.
 * @return ServerResponse JSON envelope.
 * @internal
 * @version 1.8.5
 */
std::string ServerManager::execute(
    const std::string& tool_name,
    const std::string& args_json) {

    // Check permissions first
    auto pattern = tool_name + ":" + args_to_pattern(args_json);
    if (permissions_.is_denied(tool_name, pattern)) {
        logger->warn("Permission denied: {}", tool_name);
        nlohmann::json resp;
        resp["result"] = "Error: Permission denied for " + tool_name;
        resp["directives"] = nlohmann::json::array();
        return resp.dump();
    }

    auto prefix = extract_prefix(tool_name);
    auto it = servers_.find(prefix);
    if (it == servers_.end()) {
        logger->warn("Unknown server: {}", prefix);
        nlohmann::json resp;
        resp["result"] = "Error: Unknown server '" + prefix + "'";
        resp["directives"] = nlohmann::json::array();
        return resp.dump();
    }

    auto local_name = extract_local_name(tool_name);
    return it->second->execute(local_name, args_json);
}

/**
 * @brief Check if tool is explicitly allowed.
 * @param tool_name Fully-qualified tool name.
 * @param args_json Tool arguments.
 * @return true if in allow list.
 * @internal
 * @version 1.8.5
 */
bool ServerManager::is_explicitly_allowed(
    const std::string& tool_name,
    const std::string& args_json) const {
    auto pattern = tool_name + ":" + args_to_pattern(args_json);
    return permissions_.is_allowed(tool_name, pattern);
}

/**
 * @brief Generate permission pattern via server delegation.
 * @param tool_name Fully-qualified tool name.
 * @param args_json Tool arguments.
 * @return Permission pattern.
 * @internal
 * @version 1.8.5
 */
std::string ServerManager::get_permission_pattern(
    const std::string& tool_name,
    const std::string& args_json) const {
    auto prefix = extract_prefix(tool_name);
    auto it = servers_.find(prefix);
    if (it != servers_.end()) {
        return it->second->get_permission_pattern(
            tool_name, args_json);
    }
    return tool_name;
}

/**
 * @brief Check if tool should skip duplicate detection.
 * @param tool_name Fully-qualified tool name.
 * @return true if skip.
 * @internal
 * @version 1.8.5
 */
bool ServerManager::skip_duplicate_check(
    const std::string& tool_name) const {
    auto prefix = extract_prefix(tool_name);
    auto it = servers_.find(prefix);
    if (it != servers_.end()) {
        auto local = extract_local_name(tool_name);
        return it->second->skip_duplicate_check(local);
    }
    return false;
}

/**
 * @brief Add a runtime permission pattern.
 * @param pattern Permission pattern.
 * @param allow true for allow list.
 * @internal
 * @version 1.8.5
 */
void ServerManager::add_permission(
    const std::string& pattern, bool allow) {
    permissions_.add_permission(pattern, allow);
}

/**
 * @brief Shutdown all servers.
 * @internal
 * @version 1.8.5
 */
void ServerManager::shutdown() {
    logger->info("Shutting down {} MCP servers", servers_.size());
    servers_.clear();
}

/**
 * @brief Extract server prefix from fully-qualified tool name.
 * @param tool_name E.g., "filesystem.read_file".
 * @return E.g., "filesystem".
 * @internal
 * @version 1.8.5
 */
std::string ServerManager::extract_prefix(
    const std::string& tool_name) {
    auto dot = tool_name.find('.');
    if (dot == std::string::npos) {
        return tool_name;
    }
    return tool_name.substr(0, dot);
}

/**
 * @brief Extract local tool name from fully-qualified name.
 * @param tool_name E.g., "filesystem.read_file".
 * @return E.g., "read_file".
 * @internal
 * @version 1.8.5
 */
std::string ServerManager::extract_local_name(
    const std::string& tool_name) {
    auto dot = tool_name.find('.');
    if (dot == std::string::npos) {
        return tool_name;
    }
    return tool_name.substr(dot + 1);
}

/**
 * @brief Build args-to-pattern string for permission matching.
 * @param args_json JSON arguments.
 * @return Pattern string (first arg value, or "*").
 * @internal
 * @version 1.8.5
 */
std::string ServerManager::args_to_pattern(
    const std::string& args_json) {
    if (args_json.empty() || args_json == "{}") {
        return "*";
    }
    try {
        auto j = nlohmann::json::parse(args_json);
        if (j.is_object() && !j.empty()) {
            auto first = j.begin();
            if (first->is_string()) {
                return first->get<std::string>();
            }
        }
    } catch (...) {
        // Parse failure — treat as wildcard
    }
    return "*";
}

} // namespace entropic
