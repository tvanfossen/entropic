// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file i_mcp_server.h
 * @brief Pure C interface contract for MCP server plugins.
 *
 * Every MCP server plugin (.so) implements this interface. The
 * ServerManager discovers plugins via dlopen and calls these
 * functions through the opaque handle.
 *
 * @par Memory ownership
 * - Strings returned by list_tools and execute are caller-owned.
 *   Free with entropic_free().
 * - Strings returned by name are server-owned (valid for handle lifetime).
 * - Input strings (tool_name, args_json, config_json) are borrowed
 *   for the duration of the call only.
 *
 * @par Plugin export requirements
 * Every MCP server .so must export:
 * @code
 *   extern "C" ENTROPIC_EXPORT int entropic_plugin_api_version();
 *   extern "C" ENTROPIC_EXPORT entropic_mcp_server_t entropic_create_server();
 * @endcode
 *
 * @version 1.8.5
 */

#pragma once

#include <stddef.h>
#include <entropic/types/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to an MCP server instance.
 * @version 1.8.5
 */
typedef struct entropic_mcp_server* entropic_mcp_server_t;

/**
 * @brief Get the server name.
 * @param server Server handle.
 * @return Null-terminated server name string. Owned by the server.
 * @version 1.8.5
 */
const char* entropic_mcp_server_name(entropic_mcp_server_t server);

/**
 * @brief List tools as JSON array string.
 * @param server Server handle.
 * @return JSON string of tool definitions. Caller must free with entropic_free().
 * @version 1.8.5
 */
char* entropic_mcp_server_list_tools(entropic_mcp_server_t server);

/**
 * @brief Execute a tool and return ServerResponse JSON envelope.
 * @param server Server handle.
 * @param tool_name Tool name (without server prefix).
 * @param args_json JSON string of arguments.
 * @return JSON string: {"result":"...","directives":[...]}.
 *         Caller must free with entropic_free().
 *         Empty directives array when tool has no side effects.
 * @version 1.8.5
 */
char* entropic_mcp_server_execute(entropic_mcp_server_t server, const char* tool_name, const char* args_json);

/**
 * @brief Configure a server instance after creation.
 * @param server Server handle.
 * @param config_json JSON configuration string.
 * @return ENTROPIC_OK on success.
 * @version 1.8.5
 *
 * Some servers need construction parameters (root_dir, config).
 * The entropic_create_server() signature is parameterless for ABI
 * uniformity. Per-server configuration is passed via this call.
 */
entropic_error_t entropic_mcp_server_configure(
    entropic_mcp_server_t server,
    const char* config_json);

/**
 * @brief Set the working directory for a server.
 * @param server Server handle.
 * @param path Working directory path.
 * @return ENTROPIC_OK on success.
 * @version 1.8.5
 *
 * Base class default is no-op. Directory-aware servers (filesystem,
 * bash, git) implement this. Enables v1.8.6 ScopedWorktree to swap
 * directories across .so boundaries without breaking isolation.
 */
entropic_error_t entropic_mcp_server_set_working_dir(
    entropic_mcp_server_t server,
    const char* path);

/**
 * @brief Destroy a server instance.
 * @param server Server handle to destroy. NULL is a safe no-op.
 * @version 1.8.5
 */
void entropic_mcp_server_destroy(entropic_mcp_server_t server);

/**
 * @brief Free a string allocated by the server.
 * @param ptr Pointer returned by list_tools or execute. NULL is a safe no-op.
 * @version 1.8.5
 */
void entropic_free(void* ptr);

/**
 * @brief Read-only engine state provider for introspection tools.
 *
 * Callback struct passed to EntropicServer for entropic.diagnose
 * and entropic.inspect. Each callback returns a JSON string
 * allocated with malloc/strdup. Caller must free with free().
 *
 * The facade implements these callbacks by querying the appropriate
 * subsystem (config loader, prompt manager, server manager, etc.).
 *
 * @version 1.9.12
 */
typedef struct {
    /** @brief Get current engine configuration as JSON. */
    char* (*get_config)(void* user_data);

    /** @brief Get loaded identities as JSON array. */
    char* (*get_identities)(void* user_data);

    /** @brief Get available tools as JSON array. */
    char* (*get_tools)(void* user_data);

    /**
     * @brief Get recent tool call history as JSON array.
     * @param max_entries Maximum entries to return (0 = all).
     */
    char* (*get_history)(int max_entries, void* user_data);

    /** @brief Get engine state as JSON. */
    char* (*get_state)(void* user_data);

    /** @brief Get engine metrics as JSON. */
    char* (*get_metrics)(void* user_data);

    /**
     * @brief Get bundled documentation as text.
     * @param section Section name (NULL = full doc).
     */
    char* (*get_docs)(const char* section, void* user_data);

    /** @brief Opaque user data passed to all callbacks. */
    void* user_data;
} entropic_state_provider_t;

#ifdef __cplusplus
}
#endif

/**
 * @brief Current MCP plugin API version.
 *
 * Bumped when MCPServerBase or ToolBase virtual method signatures change.
 * A plugin built against an older vtable loaded into a newer engine will
 * be rejected with ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH.
 *
 * @version 1.8.5
 */
#define ENTROPIC_MCP_PLUGIN_API_VERSION 1
