/**
 * @file server_manager.h
 * @brief MCP server lifecycle management and tool routing.
 *
 * Manages registered in-process servers. Routes tool calls by server
 * prefix (e.g., "filesystem.read_file" → filesystem server).
 * Plugin .so discovery via dlopen with API version checking.
 *
 * @version 1.8.5
 */

#pragma once

#include <entropic/mcp/permission_manager.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/config.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Manages MCP server instances and routes tool calls.
 *
 * Owns server instances, handles tool routing by server prefix,
 * delegates permission checking to PermissionManager, and provides
 * server-class-level methods (get_permission_pattern, skip_duplicate_check).
 *
 * @version 1.8.5
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
     * @brief Shutdown all servers.
     * @version 1.8.5
     */
    void shutdown();

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
    std::map<std::string, std::unique_ptr<MCPServerBase>> servers_; ///< Name → server
};

} // namespace entropic
