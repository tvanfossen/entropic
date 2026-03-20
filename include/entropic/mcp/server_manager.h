/**
 * @file server_manager.h
 * @brief MCP server lifecycle management and tool routing.
 *
 * Manages both in-process servers (v1.8.5) and external MCP servers
 * (v1.8.7). Routes tool calls by server prefix. External servers
 * connect via stdio or SSE transports.
 *
 * @version 1.8.7
 */

#pragma once

#include <entropic/mcp/external_client.h>
#include <entropic/mcp/health_monitor.h>
#include <entropic/mcp/mcp_json_discovery.h>
#include <entropic/mcp/permission_manager.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/config.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Runtime state of a connected MCP server.
 * @version 1.8.7
 */
struct ServerInfo {
    std::string name;                              ///< Server name (unique key)
    std::string transport;                         ///< "stdio" | "sse" | "in_process"
    std::string url;                               ///< SSE URL (empty otherwise)
    std::string command;                           ///< Stdio command (empty otherwise)
    std::string status;                            ///< "connected" | "disconnected" | "error" | "reconnecting"
    std::vector<std::string> tools;                ///< Registered tool names (prefixed)
    std::string source;                            ///< "builtin" | "config" | "mcp_json" | "runtime"
    std::chrono::system_clock::time_point connected_at; ///< Connection timestamp
    int reconnect_attempts{0};                     ///< Current reconnect attempt count
};

/**
 * @brief Manages MCP server instances and routes tool calls.
 *
 * Owns both in-process servers (v1.8.5) and external MCP clients
 * (v1.8.7). Tool routing by server prefix is uniform across both.
 * External servers discovered from .mcp.json and YAML config.
 *
 * @version 1.8.7
 */
class ServerManager {
public:
    /**
     * @brief Construct with permission config and project directory.
     * @param permissions Permission configuration.
     * @param project_dir Project root directory.
     * @version 1.8.5
     */
    ServerManager(const PermissionsConfig& permissions,
                  const std::filesystem::path& project_dir);

    /**
     * @brief Register a built-in server (in-process, ownership transferred).
     * @param server Server instance.
     * @version 1.8.5
     */
    void register_server(std::unique_ptr<MCPServerBase> server);

    /**
     * @brief Initialize all registered servers.
     * @version 1.8.5
     */
    void initialize();

    /**
     * @brief List all tools from all connected servers.
     * @return JSON array string of tool definitions.
     * @version 1.8.5
     */
    std::string list_tools() const;

    /**
     * @brief Execute a tool call via the appropriate server.
     * @param tool_name Fully-qualified name (e.g., "filesystem.read_file").
     * @param args_json JSON arguments string.
     * @return ServerResponse JSON envelope.
     * @version 1.8.5
     *
     * Checks permissions before execution. Returns error JSON if denied
     * or server not found.
     */
    std::string execute(const std::string& tool_name,
                        const std::string& args_json);

    /**
     * @brief Check if tool is explicitly allowed (skip prompting).
     * @param tool_name Fully-qualified tool name.
     * @param args_json Tool arguments as JSON.
     * @return true if in allow list.
     * @version 1.8.5
     */
    bool is_explicitly_allowed(const std::string& tool_name,
                               const std::string& args_json) const;

    /**
     * @brief Generate permission pattern via server class delegation.
     * @param tool_name Fully-qualified tool name.
     * @param args_json Tool arguments as JSON.
     * @return Permission pattern string.
     * @version 1.8.5
     */
    std::string get_permission_pattern(const std::string& tool_name,
                                       const std::string& args_json) const;

    /**
     * @brief Check if tool should skip duplicate detection.
     * @param tool_name Fully-qualified tool name.
     * @return true if duplicate check should be skipped.
     * @version 1.8.5
     */
    bool skip_duplicate_check(const std::string& tool_name) const;

    /**
     * @brief Add a runtime permission pattern.
     * @param pattern Permission pattern.
     * @param allow true for allow list, false for deny list.
     * @version 1.8.5
     */
    void add_permission(const std::string& pattern, bool allow);

    /**
     * @brief Shutdown all servers (in-process + external).
     * @version 1.8.7
     */
    void shutdown();

    /* ── v1.8.7: External server methods ───────────────── */

    /**
     * @brief Connect to an external MCP server at runtime.
     * @param name Unique server name.
     * @param command Stdio command (mutually exclusive with url).
     * @param args Stdio command arguments.
     * @param url SSE endpoint URL (mutually exclusive with command).
     * @return List of tool names registered from the server.
     * @version 1.8.7
     */
    std::vector<std::string> connect_external_server(
        const std::string& name,
        const std::string& command = "",
        const std::vector<std::string>& args = {},
        const std::string& url = "");

    /**
     * @brief Disconnect and remove an external server.
     * @param name Server name to disconnect.
     * @version 1.8.7
     */
    void disconnect_external_server(const std::string& name);

    /**
     * @brief Get snapshot of all servers with current status.
     * @return Map of server name to ServerInfo.
     * @version 1.8.7
     */
    std::map<std::string, ServerInfo> list_server_info() const;

    /**
     * @brief Process pending health events (call from engine loop).
     * @version 1.8.7
     */
    void process_health_events();

    /**
     * @brief Set MCP config for external server initialization.
     * @param config MCP configuration with external_servers.
     * @version 1.8.7
     */
    void set_mcp_config(const MCPConfig& config);

private:
    /**
     * @brief Extract server prefix from fully-qualified tool name.
     * @param tool_name Fully-qualified name (e.g., "filesystem.read_file").
     * @return Server prefix (e.g., "filesystem").
     * @version 1.8.5
     */
    static std::string extract_prefix(const std::string& tool_name);

    /**
     * @brief Extract local tool name from fully-qualified name.
     * @param tool_name Fully-qualified name (e.g., "filesystem.read_file").
     * @return Local name (e.g., "read_file").
     * @version 1.8.5
     */
    static std::string extract_local_name(const std::string& tool_name);

    /**
     * @brief Build args-to-pattern string for permission matching.
     * @param args_json JSON arguments.
     * @return Pattern string.
     * @version 1.8.5
     */
    static std::string args_to_pattern(const std::string& args_json);

    PermissionManager permissions_;                                 ///< Permission manager
    std::filesystem::path project_dir_;                             ///< Project root
    std::map<std::string, std::unique_ptr<MCPServerBase>> servers_; ///< Name → in-process server

    /* ── v1.8.7: External server state ─────────────────── */
    std::map<std::string, std::unique_ptr<ExternalMCPClient>> external_clients_; ///< Name → external client
    std::map<std::string, ServerInfo> server_info_;                               ///< All server info
    std::unique_ptr<HealthMonitor> health_monitor_;                               ///< Health monitoring
    std::unique_ptr<MCPJsonDiscovery> mcp_json_discovery_;                        ///< .mcp.json discovery
    MCPConfig mcp_config_;                                                        ///< MCP configuration

    /**
     * @brief Initialize external servers from config + .mcp.json.
     * @utility
     * @version 1.8.7
     */
    void initialize_external_servers();

    /**
     * @brief Route a tool call to the correct server.
     * @param tool_name Fully-qualified name.
     * @param args_json JSON arguments.
     * @return ServerResponse JSON envelope.
     * @utility
     * @version 1.8.7
     */
    std::string route_tool_call(const std::string& tool_name,
                                 const std::string& args_json);

    /**
     * @brief Route a tool call to an external client.
     * @param client External client.
     * @param tool_name Full tool name.
     * @param local_name Local tool name.
     * @param args_json JSON arguments.
     * @return ServerResponse JSON envelope.
     * @utility
     * @version 1.8.7
     */
    std::string route_external_call(
        ExternalMCPClient* client,
        const std::string& tool_name,
        const std::string& local_name,
        const std::string& args_json);

    /**
     * @brief Connect an external client and register info.
     * @param name Server name.
     * @param client Client to connect.
     * @param source Source identifier.
     * @param url SSE URL (may be empty).
     * @param command Stdio command (may be empty).
     * @utility
     * @version 1.8.7
     */
    void connect_and_register_external(
        const std::string& name,
        std::unique_ptr<ExternalMCPClient> client,
        const std::string& source,
        const std::string& url,
        const std::string& command);

    /**
     * @brief Create an ExternalMCPClient from config entry.
     * @param name Server name.
     * @param entry Config entry.
     * @return Unique pointer to client.
     * @utility
     * @version 1.8.7
     */
    std::unique_ptr<ExternalMCPClient> create_external_client(
        const std::string& name,
        const ExternalServerEntry& entry);

    /**
     * @brief Create an ExternalMCPClient from discovery config.
     * @param config Discovery config entry.
     * @return Unique pointer to client.
     * @utility
     * @version 1.8.7
     */
    std::unique_ptr<ExternalMCPClient> create_external_client(
        const ExternalServerConfig& config);

    /**
     * @brief Build error response JSON for disconnected server.
     * @param tool_name Full tool name.
     * @param server_name Server name.
     * @return ServerResponse JSON string.
     * @utility
     * @version 1.8.7
     */
    static std::string disconnected_error(
        const std::string& tool_name,
        const std::string& server_name);
};

} // namespace entropic
