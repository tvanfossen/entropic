// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file server_manager.cpp
 * @brief ServerManager implementation.
 * @version 1.8.7
 */

#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/servers/bash.h>
#include <entropic/mcp/servers/diagnostics.h>
#include <entropic/mcp/servers/entropic_server.h>
#include <entropic/mcp/servers/filesystem.h>
#include <entropic/mcp/servers/git.h>
#include <entropic/mcp/servers/web.h>
#include <entropic/mcp/transport_stdio.h>
#include <entropic/mcp/transport_sse.h>
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
 * @brief Register built-in servers based on config flags.
 * @param mcp MCP config with enable flags.
 * @param tier_names Tier names for entropic server.
 * @param data_dir Bundled data directory.
 * @internal
 * @version 2.0.1
 */
void ServerManager::init_builtins(
    const MCPConfig& mcp,
    const std::vector<std::string>& tier_names,
    const std::string& data_dir) {
    if (mcp.enable_entropic) {
        register_server(std::make_unique<EntropicServer>(
            tier_names, data_dir));
    }
    if (mcp.enable_filesystem) {
        register_server(std::make_unique<FilesystemServer>(
            project_dir_, mcp.filesystem, data_dir));
    }
    if (mcp.enable_bash) {
        register_server(std::make_unique<BashServer>(
            project_dir_, data_dir));
    }
    if (mcp.enable_git) {
        register_server(std::make_unique<GitServer>(
            project_dir_, data_dir));
    }
    if (mcp.enable_diagnostics) {
        register_server(std::make_unique<DiagnosticsServer>(
            project_dir_, data_dir));
    }
    if (mcp.enable_web) {
        register_server(std::make_unique<WebServer>(data_dir));
    }
}

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
 * @brief Initialize all registered servers + external connections.
 * @internal
 * @version 1.8.7
 */
void ServerManager::initialize() {
    logger->info("Initializing {} in-process MCP servers",
                 servers_.size());
    for (auto& [name, server] : servers_) {
        logger->info("Server '{}' ready", name);
    }

    initialize_external_servers();
}

/**
 * @brief List tools from all servers (in-process + external).
 * @return JSON array string.
 * @internal
 * @version 2.0.0
 */
std::string ServerManager::list_tools() const {
    auto all = nlohmann::json::array();

    // In-process servers
    for (const auto& [name, server] : servers_) {
        auto tools_json = server->list_tools();
        auto tools = nlohmann::json::parse(tools_json);
        for (auto& tool : tools) {
            std::string orig_name = tool["name"];
            tool["name"] = name + "." + orig_name;
            all.push_back(std::move(tool));
        }
    }

    // External servers (tools already prefixed by ExternalMCPClient)
    for (const auto& [name, client] : external_clients_) {
        if (!client->is_connected()) {
            continue;
        }
        auto tools_json = client->list_tools();
        auto tools = nlohmann::json::parse(tools_json);
        for (auto& tool : tools) {
            all.push_back(std::move(tool));
        }
    }

    logger->info("Tool list: {} tools from {} server(s) + {} external",
                 all.size(), servers_.size(),
                 external_clients_.size());
    return all.dump();
}

/**
 * @brief Execute a tool call via the appropriate server.
 * @param tool_name Fully-qualified name.
 * @param args_json JSON arguments.
 * @return ServerResponse JSON envelope.
 * @internal
 * @version 1.8.7
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

    return route_tool_call(tool_name, args_json);
}

/**
 * @brief Get a registered in-process server by name.
 * @param name Server name.
 * @return Server pointer, or nullptr if not found.
 * @internal
 * @version 2.0.6
 */
MCPServerBase* ServerManager::get_server(const std::string& name) const {
    auto it = servers_.find(name);
    return (it != servers_.end()) ? it->second.get() : nullptr;
}

/**
 * @brief List all registered server names.
 * @return Server names (in-process + external).
 * @internal
 * @version 2.0.6
 */
std::vector<std::string> ServerManager::server_names() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : servers_) {
        names.push_back(name);
    }
    for (const auto& [name, _] : external_clients_) {
        names.push_back(name);
    }
    return names;
}

/**
 * @brief Get the JSON Schema for a tool's input parameters.
 * @param tool_name Fully-qualified tool name.
 * @return input_schema JSON string, or empty if tool not found.
 * @internal
 * @version 2.0.6
 */
std::string ServerManager::get_tool_schema(
    const std::string& tool_name) const {
    auto prefix = extract_prefix(tool_name);
    auto local_name = extract_local_name(tool_name);
    auto it = servers_.find(prefix);
    if (it != servers_.end()) {
        auto* tool = it->second->registry().get_tool(local_name);
        if (tool != nullptr) {
            return tool->definition().input_schema;
        }
    }
    return "";
}

/**
 * @brief Route a tool call to the correct server (in-process or external).
 * @param tool_name Fully-qualified name.
 * @param args_json JSON arguments.
 * @return ServerResponse JSON envelope.
 * @utility
 * @version 1.8.7
 */
std::string ServerManager::route_tool_call(
    const std::string& tool_name,
    const std::string& args_json) {

    auto prefix = extract_prefix(tool_name);
    auto local_name = extract_local_name(tool_name);

    // Try in-process server first
    auto it = servers_.find(prefix);
    if (it != servers_.end()) {
        return it->second->execute(local_name, args_json);
    }

    // Try external client
    auto ext_it = external_clients_.find(prefix);
    if (ext_it != external_clients_.end()) {
        return route_external_call(
            ext_it->second.get(), tool_name, local_name, args_json);
    }

    logger->warn("Unknown server: {}", prefix);
    nlohmann::json resp;
    resp["result"] = "Error: Unknown server '" + prefix + "'";
    resp["directives"] = nlohmann::json::array();
    return resp.dump();
}

/**
 * @brief Route a tool call to an external client (with disconnect check).
 * @param client External client pointer.
 * @param tool_name Full tool name (for error messages).
 * @param local_name Local tool name.
 * @param args_json JSON arguments.
 * @return ServerResponse JSON envelope.
 * @utility
 * @version 1.8.7
 */
std::string ServerManager::route_external_call(
    ExternalMCPClient* client,
    const std::string& tool_name,
    const std::string& local_name,
    const std::string& args_json) {

    if (!client->is_connected()) {
        auto prefix = extract_prefix(tool_name);
        return disconnected_error(tool_name, prefix);
    }
    return client->execute(local_name, args_json);
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
 * @brief Get the required access level for a tool.
 * @param tool_name Fully-qualified tool name.
 * @return MCPAccessLevel required, or WRITE if not found.
 * @internal
 * @version 1.9.4
 */
MCPAccessLevel ServerManager::get_required_access_level(
    const std::string& tool_name) const {
    auto prefix = extract_prefix(tool_name);
    auto it = servers_.find(prefix);
    if (it != servers_.end()) {
        auto local = extract_local_name(tool_name);
        auto* tool = it->second->registry().get_tool(local);
        if (tool != nullptr) {
            return tool->required_access_level();
        }
    }
    return MCPAccessLevel::WRITE;  // Safe default
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
 * @brief Shutdown all servers (in-process + external).
 * @internal
 * @version 1.8.7
 */
void ServerManager::shutdown() {
    // Stop health monitor first
    if (health_monitor_) {
        health_monitor_->stop();
    }

    // Disconnect external clients
    for (auto& [name, client] : external_clients_) {
        client->disconnect();
    }
    external_clients_.clear();

    // Destroy in-process servers
    logger->info("Shutting down {} MCP servers", servers_.size());
    servers_.clear();
    server_info_.clear();
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

// ── v1.8.7: External server methods ─────────────────────

/**
 * @brief Set MCP config for external server initialization.
 * @param config MCP configuration.
 * @internal
 * @version 1.8.7
 */
void ServerManager::set_mcp_config(const MCPConfig& config) {
    mcp_config_ = config;
}

/**
 * @brief Signal every external client to cancel its in-flight tool.
 *
 * Called by AgentEngine::interrupt() via a facade-wired callback so
 * tool dispatches to docs_server.py / bash / git unwind within ~100ms
 * of Ctrl+C instead of running to completion. (P1-10, 2.0.6-rc16)
 *
 * @internal
 * @version 2.0.6-rc16
 */
void ServerManager::interrupt_external_tools() {
    for (auto& [_, client] : external_clients_) {
        if (client) { client->interrupt(); }
    }
}

/**
 * @brief Initialize external servers from config + .mcp.json.
 * @utility
 * @version 1.8.7
 */
void ServerManager::initialize_external_servers() {
    // Create .mcp.json discovery
    mcp_json_discovery_ = std::make_unique<MCPJsonDiscovery>(
        project_dir_);

    // YAML config external_servers
    for (const auto& [name, entry] : mcp_config_.external_servers) {
        auto client = create_external_client(name, entry);
        connect_and_register_external(name, std::move(client),
                                      "config", entry.url,
                                      entry.command);
    }

    // .mcp.json discovery
    std::set<std::string> existing;
    for (const auto& [name, _] : servers_) {
        existing.insert(name);
    }
    for (const auto& [name, _] : external_clients_) {
        existing.insert(name);
    }

    auto discovered = mcp_json_discovery_->discover(existing);
    for (const auto& cfg : discovered) {
        auto client = create_external_client(cfg);
        connect_and_register_external(cfg.name, std::move(client),
                                      "mcp_json", cfg.url,
                                      cfg.command);
    }

    // Start health monitor
    health_monitor_ = std::make_unique<HealthMonitor>(
        ReconnectPolicy(mcp_config_.reconnect),
        mcp_config_.health_check_interval_ms);

    for (auto& [name, client] : external_clients_) {
        health_monitor_->watch(name, client.get());
    }
    if (!external_clients_.empty()) {
        health_monitor_->start();
    }

    logger->info("External MCP: {} servers connected",
                 external_clients_.size());
}

/**
 * @brief Connect an external client and register it.
 * @param name Server name.
 * @param client Client to connect.
 * @param source Source identifier.
 * @param url SSE URL (may be empty).
 * @param command Stdio command (may be empty).
 * @utility
 * @version 1.8.7
 */
void ServerManager::connect_and_register_external(
    const std::string& name,
    std::unique_ptr<ExternalMCPClient> client,
    const std::string& source,
    const std::string& url,
    const std::string& command) {

    ServerInfo info;
    info.name = name;
    info.transport = url.empty() ? "stdio" : "sse";
    info.url = url;
    info.command = command;
    info.source = source;
    info.status = "disconnected";

    bool ok = client->connect();
    if (ok) {
        info.status = "connected";
        info.connected_at = std::chrono::system_clock::now();
    } else {
        info.status = "error";
        logger->error("Failed to connect external server '{}'", name);
    }

    server_info_[name] = info;
    external_clients_[name] = std::move(client);
}

/**
 * @brief Connect to an external MCP server at runtime.
 * @param name Server name.
 * @param command Stdio command.
 * @param args Stdio args.
 * @param url SSE URL.
 * @return Registered tool names.
 * @internal
 * @version 1.8.7
 */
std::vector<std::string> ServerManager::connect_external_server(
    const std::string& name,
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& url) {

    if (servers_.count(name) > 0 ||
        external_clients_.count(name) > 0) {
        logger->warn("Server '{}' already registered", name);
        return {};
    }

    std::unique_ptr<Transport> transport;
    if (!url.empty()) {
        transport = std::make_unique<SSETransport>(url);
    } else {
        transport = std::make_unique<StdioTransport>(
            command, args);
    }

    auto client = std::make_unique<ExternalMCPClient>(
        name, std::move(transport));

    connect_and_register_external(name, std::move(client),
                                  "runtime", url, command);

    auto& registered = external_clients_[name];
    if (health_monitor_) {
        health_monitor_->watch(name, registered.get());
    }

    // Parse tool names from cached list
    std::vector<std::string> tool_names;
    auto tools_json = registered->list_tools();
    try {
        auto tools = nlohmann::json::parse(tools_json);
        for (const auto& t : tools) {
            tool_names.push_back(t["name"].get<std::string>());
        }
    } catch (...) {}

    return tool_names;
}

/**
 * @brief Disconnect and remove an external server.
 * @param name Server name.
 * @internal
 * @version 1.8.7
 */
void ServerManager::disconnect_external_server(
    const std::string& name) {

    auto it = external_clients_.find(name);
    if (it == external_clients_.end()) {
        logger->warn("External server '{}' not found", name);
        return;
    }

    if (health_monitor_) {
        health_monitor_->unwatch(name);
    }

    it->second->disconnect();
    external_clients_.erase(it);
    server_info_.erase(name);

    logger->info("External server '{}' disconnected", name);
}

/**
 * @brief Get snapshot of all servers with current status.
 * @return Map of name to ServerInfo.
 * @internal
 * @version 1.8.7
 */
std::map<std::string, ServerInfo>
ServerManager::list_server_info() const {
    auto result = server_info_;

    // Add in-process servers
    for (const auto& [name, _] : servers_) {
        if (result.count(name) == 0) {
            ServerInfo info;
            info.name = name;
            info.transport = "in_process";
            info.status = "connected";
            info.source = "builtin";
            result[name] = info;
        }
    }
    return result;
}

/**
 * @brief Process pending health events.
 * @internal
 * @version 1.8.7
 */
void ServerManager::process_health_events() {
    if (health_monitor_) {
        health_monitor_->process_events();
    }
}

/**
 * @brief Create ExternalMCPClient from YAML config entry.
 * @param name Server name.
 * @param entry Config entry.
 * @return Client instance.
 * @utility
 * @version 1.8.7
 */
std::unique_ptr<ExternalMCPClient>
ServerManager::create_external_client(
    const std::string& name,
    const ExternalServerEntry& entry) {

    std::unique_ptr<Transport> transport;
    if (!entry.url.empty()) {
        transport = std::make_unique<SSETransport>(entry.url);
    } else {
        std::map<std::string, std::string> env(
            entry.env.begin(), entry.env.end());
        transport = std::make_unique<StdioTransport>(
            entry.command, entry.args, std::move(env));
    }
    return std::make_unique<ExternalMCPClient>(
        name, std::move(transport));
}

/**
 * @brief Create ExternalMCPClient from discovery config.
 * @param config Discovery config.
 * @return Client instance.
 * @utility
 * @version 1.8.7
 */
std::unique_ptr<ExternalMCPClient>
ServerManager::create_external_client(
    const ExternalServerConfig& config) {

    std::unique_ptr<Transport> transport;
    if (config.transport == "sse") {
        transport = std::make_unique<SSETransport>(config.url);
    } else {
        transport = std::make_unique<StdioTransport>(
            config.command, config.args, config.env);
    }
    return std::make_unique<ExternalMCPClient>(
        config.name, std::move(transport));
}

/**
 * @brief Build error response for disconnected server.
 * @param tool_name Full tool name.
 * @param server_name Server name.
 * @return ServerResponse JSON.
 * @utility
 * @version 1.8.7
 */
std::string ServerManager::disconnected_error(
    const std::string& tool_name,
    const std::string& server_name) {

    nlohmann::json resp;
    resp["result"] = "Server '" + server_name +
                     "' is disconnected. Tool '" + tool_name +
                     "' is unavailable. Use a different approach "
                     "or try again later.";
    resp["directives"] = nlohmann::json::array();
    resp["is_error"] = true;
    return resp.dump();
}

} // namespace entropic
