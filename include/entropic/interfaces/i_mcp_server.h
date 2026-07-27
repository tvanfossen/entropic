// SPDX-License-Identifier: Apache-2.0
/**
 * @file i_mcp_server.h
 * @brief Pure C interface contract for MCP server plugins.
 *
 * Every MCP server plugin (.so) implements this interface. ServerManager
 * discovers plugins via dlopen and calls these functions through the
 * opaque handle.
 *
 * @par Loading a plugin (gh#133, v2.10.1)
 * List the `.so` under `mcp.plugins` in the engine config:
 * @code{.yaml}
 *   mcp:
 *     plugins:
 *       - /path/to/libmy_mcp_server.so
 * @endcode
 * At startup ServerManager dlopens each entry, checks
 * entropic_plugin_api_version() against ENTROPIC_MCP_PLUGIN_API_VERSION,
 * resolves the entry points below, and registers the server under the name
 * returned by entropic_mcp_server_name(). Its tools are then addressable
 * as `<name>.<tool>` exactly like a built-in server's.
 *
 * A plugin that fails to open, is missing an entry point, or reports a
 * different API version is rejected LOUDLY with
 * ENTROPIC_ERROR_PLUGIN_LOAD_FAILED / ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH
 * — never skipped silently, since a configured-but-absent plugin would
 * leave the operator's stated intent unmet with no signal.
 *
 * Before v2.10.1 this header documented the dlopen contract but no loader
 * existed; a conformant `.so` could not be loaded by anything.
 *
 * @par Memory ownership
 * - Strings returned by list_tools and execute are caller-owned.
 *   Free with entropic_free().
 * - Strings returned by name are server-owned (valid for handle lifetime).
 * - Input strings (tool_name, args_json, config_json) are borrowed
 *   for the duration of the call only.
 *
 * @par Threading (v2.10.1)
 * The engine serialises calls into a given server instance — execute(),
 * configure(), and set_working_dir() never run concurrently on the same
 * handle, so a plugin needs no internal locking for its own state. No
 * ordering is guaranteed *between* distinct handles, and the calling
 * thread is not fixed, so a plugin must not use thread-local state to
 * carry data across calls. destroy() happens-after every other call on
 * that handle.
 *
 * @par Tool descriptor shape
 * Tool definitions returned by list_tools use `inputSchema` (camelCase),
 * matching the bundled descriptors in `data/tools/&#42;/&#42;.json`:
 * @code{.json}
 *   [{"name":"echo","description":"...",
 *     "inputSchema":{"type":"object","properties":{...},"required":[...]}}]
 * @endcode
 *
 * @par Plugin export requirements
 * Every MCP server .so must export all nine entry points declared below.
 * They carry ENTROPIC_EXPORT so that a plugin defining them plainly —
 * @code
 *   extern "C" int entropic_plugin_api_version() { return 1; }
 * @endcode
 * — still exports them under `-fvisibility=hidden`, which is how most
 * plugin projects build. Without the attribute on these declarations such
 * a definition inherits hidden visibility and the resulting `.so` exports
 * nothing at all, making every dlsym in the loader fail.
 *
 * @par ABI lock
 * Function signatures and the `entropic_mcp_server_t` opaque-handle
 * type are LOCKED at API version 1. Any change to a function
 * declaration, struct member layout, or the macro/typedef contracts in
 * this header is a breaking change that requires bumping
 * `entropic_plugin_api_version()`. Comment-only changes are safe.
 * Cross-version compatibility test: a plugin built against 2.1.4
 * headers MUST `dlopen` cleanly into a 2.1.5 engine (verified
 * comment-only diff in `gh release v2.1.5` review).
 *
 * The v2.10.1 addition of ENTROPIC_EXPORT to these declarations is NOT a
 * version bump: it changes symbol visibility only, leaving every signature,
 * calling convention, and the opaque-handle type untouched. A plugin built
 * against pre-2.10.1 headers that exported its entry points by other means
 * (its own visibility attribute, or a version script) dlopens into a
 * v2.10.1 engine unchanged.
 *
 * @version 2.10.1
 */

#pragma once

#include <stddef.h>
#include <entropic/entropic_export.h>
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
 * @brief Report the plugin API version this plugin was built against.
 *
 * Implemented BY THE PLUGIN, called by the engine's loader. Return
 * ENTROPIC_MCP_PLUGIN_API_VERSION; a plugin reporting anything else is
 * rejected with ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH and not registered.
 *
 * Declared here (v2.10.1) rather than described only in prose so that
 * plugin definitions pick up ENTROPIC_EXPORT from this header.
 *
 * @return Plugin API version.
 * @version 2.10.1
 */
ENTROPIC_EXPORT int entropic_plugin_api_version(void);

/**
 * @brief Construct a server instance.
 *
 * Implemented BY THE PLUGIN, called by the engine's loader once per
 * configured `.so`. Parameterless for ABI uniformity — pass construction
 * parameters via entropic_mcp_server_configure() instead. The returned
 * handle is owned by the engine and released with
 * entropic_mcp_server_destroy().
 *
 * Declared here (v2.10.1) rather than described only in prose so that
 * plugin definitions pick up ENTROPIC_EXPORT from this header.
 *
 * @return Opaque server handle, or NULL on failure.
 * @version 2.10.1
 */
ENTROPIC_EXPORT entropic_mcp_server_t entropic_create_server(void);

/**
 * @brief Get the server name.
 * @param server Server handle.
 * @return Null-terminated server name string. Owned by the server.
 * @version 1.8.5
 */
ENTROPIC_EXPORT const char* entropic_mcp_server_name(entropic_mcp_server_t server);

/**
 * @brief List tools as JSON array string.
 * @param server Server handle.
 * @return JSON string of tool definitions. Caller must free with entropic_free().
 * @version 1.8.5
 */
ENTROPIC_EXPORT char* entropic_mcp_server_list_tools(entropic_mcp_server_t server);

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
ENTROPIC_EXPORT char* entropic_mcp_server_execute(entropic_mcp_server_t server, const char* tool_name, const char* args_json);

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
ENTROPIC_EXPORT entropic_error_t entropic_mcp_server_configure(
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
 * bash, git) implement this. Enables ScopedSandbox (v2.1.5; formerly
 * ScopedWorktree) to swap directories across .so boundaries without
 * breaking isolation.
 */
ENTROPIC_EXPORT entropic_error_t entropic_mcp_server_set_working_dir(
    entropic_mcp_server_t server,
    const char* path);

/**
 * @brief Destroy a server instance.
 * @param server Server handle to destroy. NULL is a safe no-op.
 * @version 1.8.5
 */
ENTROPIC_EXPORT void entropic_mcp_server_destroy(entropic_mcp_server_t server);

/**
 * @brief Free a string allocated by the server.
 * @param ptr Pointer returned by list_tools or execute. NULL is a safe no-op.
 * @version 1.8.5
 */
ENTROPIC_EXPORT void entropic_free(void* ptr);

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

    /**
     * @brief Search prior delegation summaries (gh#32, v2.1.6).
     *
     * Backs `entropic.followup`. Returns a JSON array of objects with
     * fields {delegation_id, target_tier, summary, completed_at},
     * containing up to `max_results` records whose summary
     * substring-matches `query`. May be NULL on engines built without
     * a storage backend; the tool surfaces a typed error in that case.
     *
     * Caller must free the returned string with entropic_free().
     *
     * @param query        Keyword/phrase to match (NULL-safe).
     * @param max_results  Maximum records to return (>=1).
     * @param user_data    Opaque provider state.
     * @return JSON string (caller frees), or NULL on error.
     * @version 2.1.6
     */
    char* (*search_delegations)(const char* query, int max_results,
                                void* user_data);

    /**
     * @brief Load a delegation's full child conversation (gh#32, v2.1.6).
     *
     * Backs `entropic.resume_delegation`. Returns the conversation JSON
     * previously persisted via storage_save_conversation, scoped to the
     * delegation's child_conversation_id. May be NULL when storage is
     * unavailable or the id is unknown — the tool surfaces a typed error.
     *
     * Caller must free with entropic_free().
     *
     * @param delegation_id Storage delegation id.
     * @param user_data     Opaque provider state.
     * @return JSON string (caller frees), or NULL on error.
     * @version 2.1.6
     */
    char* (*load_delegation_conversation)(const char* delegation_id,
                                          void* user_data);

    /**
     * @brief Get current VRAM residency-set snapshot (gh#57, v2.2.4).
     *
     * Backs the engine's introspection surface for prompt-time
     * decisioning ("which tier models are loaded right now"). Mirrors
     * the C ABI `entropic_residency_snapshot` JSON schema exactly.
     * May be NULL on pre-v2.2.4 engines or before configure_dir runs;
     * the tool surfaces an empty residency array in that case.
     *
     * Caller must free with entropic_free().
     *
     * @param user_data Opaque provider state.
     * @return JSON string (caller frees), or NULL on error.
     * @version 2.2.4
     */
    char* (*get_residency)(void* user_data);

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
