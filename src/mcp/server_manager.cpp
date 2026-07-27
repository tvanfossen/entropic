// SPDX-License-Identifier: Apache-2.0
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
 * @brief Load the dlopen plugins listed in mcp.plugins (gh#133).
 * @param mcp MCP config carrying the plugin path list.
 * @return ENTROPIC_OK, or the first failure's typed code.
 * @internal
 * @version 2.10.1
 */
entropic_error_t ServerManager::load_plugins(const MCPConfig& mcp) {
    auto first_error = ENTROPIC_OK;

    for (const auto& path : mcp.plugins) {
        std::unique_ptr<PluginServer> plugin;
        auto rc = PluginServer::load(path, plugin);
        if (rc != ENTROPIC_OK) {
            // Keep going: attempting every path diagnoses a whole broken
            // config in one run instead of one entry per restart.
            first_error = (first_error == ENTROPIC_OK) ? rc : first_error;
            continue;
        }

        auto name = plugin->name();
        if (servers_.count(name) > 0 || external_clients_.count(name) > 0 ||
            plugin_servers_.count(name) > 0) {
            logger->error("Plugin '{}' from {} collides with an already "
                          "registered server of the same name — refusing to "
                          "shadow it", name, path.string());
            first_error = (first_error == ENTROPIC_OK)
                              ? ENTROPIC_ERROR_PLUGIN_LOAD_FAILED
                              : first_error;
            continue;
        }

        plugin->set_working_dir(project_dir_.string());
        plugin_servers_[name] = std::move(plugin);
        logger->info("Registered plugin server: {}", name);
    }

    return first_error;
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
 * @brief Append a server's tools to an array, prefixing each name.
 * @param tools_json Server's raw tool-descriptor JSON array.
 * @param prefix Server name to prepend as `<prefix>.<tool>`.
 * @param[in,out] all Destination array.
 * @utility
 * @version 2.10.1
 */
static void append_prefixed_tools(const std::string& tools_json,
                                  const std::string& prefix,
                                  nlohmann::json& all) {
    auto tools = nlohmann::json::parse(tools_json);
    for (auto& tool : tools) {
        std::string orig_name = tool.at("name");
        tool["name"] = prefix + "." + orig_name;
        all.push_back(std::move(tool));
    }
}

/**
 * @brief Collect tools from the dlopen plugins (gh#133).
 *
 * Plugins report bare tool names exactly as an MCPServerBase does, so they
 * get the same prefixing. Their output is third-party, so a malformed
 * descriptor list is contained to its own plugin rather than emptying the
 * tool list for every server.
 *
 * @param[in,out] all Destination array.
 * @utility
 * @version 2.10.1
 */
void ServerManager::append_plugin_tools(nlohmann::json& all) const {
    for (const auto& [name, plugin] : plugin_servers_) {
        try {
            append_prefixed_tools(plugin->list_tools(), name, all);
        } catch (const std::exception& e) {
            logger->error("Plugin '{}' returned an unusable tool list: {} — "
                          "its tools are unavailable this session", name,
                          e.what());
        }
    }
}

/**
 * @brief Collect tools from connected external clients.
 * @param[in,out] all Destination array (external tools arrive pre-prefixed).
 * @utility
 * @version 2.10.1
 */
void ServerManager::append_external_tools(nlohmann::json& all) const {
    for (const auto& [name, client] : external_clients_) {
        if (!client->is_connected()) {
            continue;
        }
        auto tools = nlohmann::json::parse(client->list_tools());
        for (auto& tool : tools) {
            all.push_back(std::move(tool));
        }
    }
}

/**
 * @brief List tools from all servers (in-process + plugin + external).
 * @return JSON array string.
 * @internal
 * @version 2.10.1
 */
std::string ServerManager::list_tools() const {
    auto all = nlohmann::json::array();

    // In-process servers
    for (const auto& [name, server] : servers_) {
        append_prefixed_tools(server->list_tools(), name, all);
    }

    append_plugin_tools(all);     // gh#133 (v2.10.1)
    append_external_tools(all);

    logger->info("Tool list: {} tools from {} server(s) + {} plugin(s) + "
                 "{} external", all.size(), servers_.size(),
                 plugin_servers_.size(), external_clients_.size());
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
 * @version 2.10.1
 */
std::vector<std::string> ServerManager::server_names() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : servers_) {
        names.push_back(name);
    }
    for (const auto& [name, _] : plugin_servers_) {  // gh#133 (v2.10.1)
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
 * @version 2.10.1
 */
std::string ServerManager::get_tool_schema(
    const std::string& tool_name) const {
    auto prefix = extract_prefix(tool_name);
    auto local_name = extract_local_name(tool_name);
    auto it = servers_.find(prefix);
    if (it != servers_.end()) {
        auto* tool = it->second->registry().get_tool(local_name);
        return (tool != nullptr) ? tool->definition().input_schema
                                 : std::string{};
    }
    // gh#133 (v2.10.1): without this a plugin's declared inputSchema would be
    // invisible to ToolExecutor::check_schema, which skips validation on an
    // empty schema — plugin tools would silently forgo the argument checking
    // every built-in server's tools get.
    return plugin_tool_schema(prefix, local_name);
}

/**
 * @brief Look up a plugin tool's inputSchema from its descriptor list.
 * @param prefix Plugin server name.
 * @param local_name Local tool name.
 * @return inputSchema JSON string, or empty when absent/unparseable.
 * @utility
 * @version 2.10.1
 */
std::string ServerManager::plugin_tool_schema(
    const std::string& prefix,
    const std::string& local_name) const {

    auto it = plugin_servers_.find(prefix);
    if (it == plugin_servers_.end()) {
        return "";
    }
    try {
        auto tools = nlohmann::json::parse(it->second->list_tools());
        for (const auto& tool : tools) {
            if (tool.value("name", std::string{}) == local_name) {
                return tool.value("inputSchema",
                                  nlohmann::json::object()).dump();
            }
        }
    } catch (const std::exception& e) {
        logger->error("Plugin '{}': cannot read schema for tool '{}': {}",
                      prefix, local_name, e.what());
    }
    return "";
}

/**
 * @brief Route a tool call to the correct server (in-process or external).
 * @param tool_name Fully-qualified name.
 * @param args_json JSON arguments.
 * @return ServerResponse JSON envelope.
 * @utility
 * @version 2.10.1
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

    // gh#133 (v2.10.1): try a loaded plugin
    auto plug_it = plugin_servers_.find(prefix);
    if (plug_it != plugin_servers_.end()) {
        return route_plugin_call(*plug_it->second, local_name, args_json);
    }

    return route_external_or_unknown(prefix, tool_name, local_name, args_json);
}

/**
 * @brief Route to an external client, or report an unknown server prefix.
 *
 * Split out of route_tool_call when gh#133 added the plugin branch — the
 * combined form exceeded the 3-return gate.
 *
 * @param prefix Server prefix.
 * @param tool_name Fully-qualified name.
 * @param local_name Local tool name.
 * @param args_json JSON arguments.
 * @return ServerResponse JSON envelope.
 * @utility
 * @version 2.10.1
 */
std::string ServerManager::route_external_or_unknown(
    const std::string& prefix,
    const std::string& tool_name,
    const std::string& local_name,
    const std::string& args_json) {

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
 * @brief Route a tool call to a loaded plugin (gh#133).
 *
 * A plugin is third-party code returning a string the engine did not
 * produce, so its envelope is validated before it reaches the agent loop —
 * a malformed response becomes a normal tool error rather than an exception
 * unwinding through the loop.
 *
 * @param plugin Target plugin.
 * @param local_name Local tool name.
 * @param args_json JSON arguments.
 * @return ServerResponse JSON envelope.
 * @utility
 * @version 2.10.1
 */
std::string ServerManager::route_plugin_call(
    PluginServer& plugin,
    const std::string& local_name,
    const std::string& args_json) {

    std::string raw;
    try {
        raw = plugin.execute(local_name, args_json);
        auto parsed = nlohmann::json::parse(raw);
        if (!parsed.is_object() || !parsed.contains("result")) {
            throw std::runtime_error("missing 'result' field");
        }
        return raw;
    } catch (const std::exception& e) {
        logger->error("Plugin '{}' returned an invalid response for tool "
                      "'{}': {} — raw response: {}",
                      plugin.name(), local_name, e.what(), raw);
        nlohmann::json resp;
        resp["result"] = "Error: plugin '" + plugin.name() +
                         "' returned an invalid response: " + e.what();
        resp["directives"] = nlohmann::json::array();
        return resp.dump();
    }
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
 * @version 2.10.1
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

    // gh#133 (v2.10.1): destroy plugin instances and dlclose their libraries
    // before the in-process servers, so nothing can call into an unmapped .so.
    logger->info("Unloading {} MCP plugin(s)", plugin_servers_.size());
    plugin_servers_.clear();

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
 * @brief Construct a Transport from a spec (single source of truth).
 *
 * Issue #9 (v2.1.4): consolidates transport construction. SSE if
 * `transport=="sse"` OR `url` is non-empty AND `command` is empty,
 * Stdio otherwise. Stdio transport receives env verbatim (caller is
 * responsible for env blocklist enforcement before populating spec).
 *
 * @internal
 * @version 2.1.5
 */
std::unique_ptr<Transport> ServerManager::make_transport(
    const ExternalServerConfig& spec) {
    bool prefer_sse = (spec.transport == "sse")
        || (!spec.url.empty() && spec.command.empty());
    if (prefer_sse) {
        return std::make_unique<SSETransport>(spec.url);
    }
    // gh#19 (v2.1.5): pass the registered server name as the display
    // label so child stderr lines and lifecycle logs identify the
    // server, not the resolved spawn command (which collides when
    // multiple servers share an entrypoint like /usr/bin/env python).
    return std::make_unique<StdioTransport>(
        spec.name, spec.command, spec.args, spec.env,
        /*default_timeout_ms=*/30000U);
}

/**
 * @brief Connect (canonical spec-based) — Issue #9, v2.1.4.
 *
 * The full ExternalServerConfig is honored. Replaces the pre-2.1.4
 * runtime path which silently dropped env (and any future spec field).
 *
 * @internal
 * @version 2.1.4
 */
std::vector<std::string> ServerManager::connect_external_server(
    const ExternalServerConfig& spec) {

    if (servers_.count(spec.name) > 0 ||
        external_clients_.count(spec.name) > 0) {
        logger->warn("Server '{}' already registered", spec.name);
        return {};
    }

    auto client = std::make_unique<ExternalMCPClient>(
        spec.name, make_transport(spec));

    connect_and_register_external(spec.name, std::move(client),
                                  "runtime", spec.url, spec.command);

    auto& registered = external_clients_[spec.name];
    if (health_monitor_) {
        health_monitor_->watch(spec.name, registered.get());
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
 * @brief Legacy primitive-args overload — forwards to spec-based API.
 *
 * Retained for in-tree callers that pre-date #9. New code should use
 * the spec-based overload.
 *
 * @internal
 * @version 2.1.4
 */
std::vector<std::string> ServerManager::connect_external_server(
    const std::string& name,
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& url) {
    ExternalServerConfig spec;
    spec.name = name;
    spec.command = command;
    spec.args = args;
    spec.url = url;
    return connect_external_server(spec);
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
 * @version 2.10.1
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

    // gh#133 (v2.10.1): plugins report their own transport so `entropic
    // inspect` distinguishes them from builtins and external processes.
    for (const auto& [name, plugin] : plugin_servers_) {
        if (result.count(name) == 0) {
            ServerInfo info;
            info.name = name;
            info.transport = "plugin";
            info.status = "connected";
            info.source = "plugin";
            info.command = plugin->path().string();
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
 *
 * Issue #9 (v2.1.4): adapts the YAML-style ExternalServerEntry into the
 * canonical ExternalServerConfig and delegates to make_transport so the
 * three pre-existing transport-construction sites all share one
 * implementation.
 *
 * @param name Server name.
 * @param entry Config entry.
 * @return Client instance.
 * @utility
 * @version 2.1.4
 */
std::unique_ptr<ExternalMCPClient>
ServerManager::create_external_client(
    const std::string& name,
    const ExternalServerEntry& entry) {
    ExternalServerConfig spec;
    spec.name = name;
    spec.command = entry.command;
    spec.args = entry.args;
    spec.env = std::map<std::string, std::string>(
        entry.env.begin(), entry.env.end());
    spec.url = entry.url;
    spec.transport = entry.url.empty() ? "stdio" : "sse";
    return std::make_unique<ExternalMCPClient>(
        name, make_transport(spec));
}

/**
 * @brief Create ExternalMCPClient from discovery config.
 *
 * Issue #9 (v2.1.4): now a thin wrapper over make_transport.
 *
 * @param config Discovery config.
 * @return Client instance.
 * @utility
 * @version 2.1.4
 */
std::unique_ptr<ExternalMCPClient>
ServerManager::create_external_client(
    const ExternalServerConfig& config) {
    return std::make_unique<ExternalMCPClient>(
        config.name, make_transport(config));
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
