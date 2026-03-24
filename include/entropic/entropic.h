/**
 * @file entropic.h
 * @brief Public C API for the Entropic inference engine.
 *
 * This is the unified facade. Most consumers link against librentropic
 * and include only this header. All functions are pure C — no C++ types
 * cross this boundary.
 *
 * @par Lifecycle
 * @code
 *   entropic_handle_t h = NULL;
 *   entropic_create(&h);
 *   entropic_configure(h, config_json);
 *   entropic_run(h, "Hello", &result);
 *   entropic_free(result);
 *   entropic_destroy(h);
 * @endcode
 *
 * @par Thread safety
 * Each function documents its thread safety class:
 * - **Thread-safe**: callable from any thread at any time.
 * - **Serialized per-handle**: one call at a time per handle;
 *   different handles are fully independent.
 * - **Single-threaded init/destroy**: must not race with any other
 *   call on the same handle.
 *
 * @par Error handling
 * Functions returning entropic_error_t set per-handle error state on
 * failure. Call entropic_last_error(handle) for the detailed message.
 *
 * @par Memory ownership
 * Functions returning char* transfer ownership to the caller.
 * Free with entropic_free(). Consumers providing buffers the engine
 * will free (e.g., hook modified_json) MUST allocate with
 * entropic_alloc(). Strings returned as const char* are owned by
 * the handle and valid until the next call on that handle.
 *
 * @version 1.9.1
 */

#pragma once

#include <stddef.h>

#include <entropic/entropic_config.h>
#include <entropic/entropic_export.h>
#include <entropic/types/error.h>
#include <entropic/types/hooks.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ────────────────────────────────────────── */

/**
 * @brief Create a new engine instance.
 *
 * Allocates and initializes an engine handle. The engine is idle until
 * entropic_configure() is called.
 *
 * @param[out] handle Pointer to receive the new handle. Set to NULL on failure.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — handle is NULL.
 *         - ENTROPIC_ERROR_OUT_OF_MEMORY — allocation failed.
 *
 * @threadsafety Single-threaded init/destroy.
 * @version 1.8.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_create(entropic_handle_t* handle);

/**
 * @brief Configure the engine from a JSON config string.
 *
 * Parses the JSON, validates against the config schema, resolves model
 * paths, and prepares the engine for operation. Does NOT load models
 * (that happens on first generate or explicit activate).
 *
 * JSON is used at the C API boundary because it's universal — any language
 * can produce it. Config FILES are YAML (parsed by ryml inside
 * librentropic-config.so). Use entropic_configure_from_file() to load
 * YAML config files directly without manual JSON conversion.
 *
 * @param handle Engine handle.
 * @param config_json JSON string with engine configuration.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — config_json is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — config validation failed.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_configure(
    entropic_handle_t handle,
    const char* config_json);

/**
 * @brief Configure engine from a YAML or JSON config file.
 *
 * Convenience wrapper that reads the file, parses it (YAML via ryml or
 * JSON via nlohmann/json based on extension), and applies the configuration.
 *
 * @param handle Engine handle.
 * @param config_path Path to YAML (.yaml/.yml) or JSON (.json) config file.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — config_path is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — config validation failed.
 *         - ENTROPIC_ERROR_IO — file not found or unreadable.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_configure_from_file(
    entropic_handle_t handle,
    const char* config_path);

/**
 * @brief Destroy an engine instance and free all resources.
 *
 * Unloads models, closes storage, destroys all child objects.
 * After this call, the handle is invalid. Passing NULL is a no-op.
 *
 * @param handle Engine handle to destroy (NULL-safe).
 *
 * @threadsafety Single-threaded init/destroy. Must not race with any
 *               other call on the same handle.
 * @version 1.8.0
 */
ENTROPIC_EXPORT void entropic_destroy(entropic_handle_t handle);

/* ── Version ──────────────────────────────────────────── */

/**
 * @brief Get the library version string.
 *
 * Returns the full semver string (e.g., "1.8.9"). The pointer has
 * static lifetime — valid for the entire process.
 *
 * @return Static null-terminated version string. Never NULL. Do NOT free.
 *
 * @threadsafety Thread-safe.
 * @version 1.8.0
 */
ENTROPIC_EXPORT const char* entropic_version(void);

/**
 * @brief Get the C API version number.
 *
 * Returns an integer starting at 1. Incremented on breaking changes:
 * signature changes, function removals, enum renumbering, or behavior
 * changes. Adding new functions does NOT increment this value.
 *
 * Compare with ENTROPIC_API_VERSION at compile time for version checks.
 *
 * @return API version integer.
 *
 * @threadsafety Thread-safe.
 * @version 1.8.0
 *
 * @see ENTROPIC_API_VERSION (compile-time constant in entropic_config.h)
 */
ENTROPIC_EXPORT int entropic_api_version(void);

/* ── Memory ───────────────────────────────────────────── */

/**
 * @brief Allocate memory using the engine's allocator.
 *
 * All cross-boundary allocations (hook modified_json, consumer-provided
 * strings that the engine will free) MUST use this function. Paired
 * with entropic_free().
 *
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure.
 *
 * @threadsafety Thread-safe.
 * @version 1.8.0
 */
ENTROPIC_EXPORT void* entropic_alloc(size_t size);

/**
 * @brief Free memory allocated by the engine or entropic_alloc().
 *
 * All char* return values that transfer ownership MUST be freed with
 * this function. Passing NULL is a no-op.
 *
 * @param ptr Pointer to free (from engine return value or entropic_alloc).
 *
 * @threadsafety Thread-safe.
 * @version 1.8.0
 */
ENTROPIC_EXPORT void entropic_free(void* ptr);

/* ── Execution ────────────────────────────────────────── */

/**
 * @brief Synchronous agentic loop.
 *
 * Runs the full agentic loop: generate, parse tool calls, execute tools,
 * re-generate until complete. Blocks until the loop finishes or is
 * interrupted via entropic_interrupt().
 *
 * @param handle Engine handle.
 * @param input User message (null-terminated).
 * @param[out] result_json JSON result string. Caller must free with entropic_free().
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_STATE — engine not configured.
 *         - ENTROPIC_ERROR_GENERATE_FAILED — inference error.
 *         - ENTROPIC_ERROR_ALREADY_RUNNING — another run in progress.
 *         - ENTROPIC_ERROR_INTERRUPTED — cancelled via entropic_interrupt().
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.4
 *
 * @par Memory ownership
 * Caller must free *result_json with entropic_free().
 */
ENTROPIC_EXPORT entropic_error_t entropic_run(
    entropic_handle_t handle,
    const char* input,
    char** result_json);

/**
 * @brief Streaming agentic loop with token callback.
 *
 * Same as entropic_run() but invokes on_token for each generated token.
 * The token pointer is valid only for the callback duration — copy if
 * retention is needed.
 *
 * @param handle Engine handle.
 * @param input User message (null-terminated).
 * @param on_token Called for each generated token. Must not call back
 *        into the entropic API (deadlock risk).
 * @param user_data Forwarded to on_token.
 * @param cancel_flag Pointer to int. Set to non-zero to cancel. May be NULL.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_STATE — engine not configured.
 *         - ENTROPIC_ERROR_GENERATE_FAILED — inference error.
 *         - ENTROPIC_ERROR_ALREADY_RUNNING — another run in progress.
 *         - ENTROPIC_ERROR_CANCELLED — cancelled via cancel_flag.
 *         - ENTROPIC_ERROR_INTERRUPTED — cancelled via entropic_interrupt().
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.4
 */
ENTROPIC_EXPORT entropic_error_t entropic_run_streaming(
    entropic_handle_t handle,
    const char* input,
    void (*on_token)(const char* token, size_t len, void* user_data),
    void* user_data,
    int* cancel_flag);

/**
 * @brief Interrupt a running generation.
 *
 * Signals the engine to abort the current entropic_run() or
 * entropic_run_streaming() call. The interrupted call returns
 * ENTROPIC_ERROR_INTERRUPTED. If nothing is running, returns
 * ENTROPIC_ERROR_NOT_RUNNING.
 *
 * @param handle Engine handle.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_NOT_RUNNING — nothing to interrupt.
 *
 * @threadsafety Thread-safe. Designed for cross-thread cancellation.
 * @version 1.8.4
 */
ENTROPIC_EXPORT entropic_error_t entropic_interrupt(entropic_handle_t handle);

/* ── External MCP Servers (v1.8.7) ───────────────────── */

/**
 * @brief Register an external MCP server at runtime.
 *
 * Connects to the server immediately. For stdio: spawns child process.
 * For SSE: connects to HTTP endpoint.
 *
 * @param handle Engine handle.
 * @param name Unique server name (null-terminated).
 * @param config_json JSON configuration:
 *   Stdio: {"command":"...","args":[...],"env":{...}}
 *   SSE:   {"url":"http://..."}
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — name or config_json is NULL.
 *         - ENTROPIC_ERROR_SERVER_ALREADY_EXISTS — name already registered.
 *         - ENTROPIC_ERROR_CONNECTION_FAILED — transport connect failed.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.7
 */
ENTROPIC_EXPORT entropic_error_t entropic_register_mcp_server(
    entropic_handle_t handle,
    const char* name,
    const char* config_json);

/**
 * @brief Deregister an external MCP server.
 *
 * Disconnects and removes the server. In-progress tool calls
 * are not interrupted but subsequent calls will fail.
 *
 * @param handle Engine handle.
 * @param name Server name to remove (null-terminated).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — name is NULL.
 *         - ENTROPIC_ERROR_SERVER_NOT_FOUND — name not registered.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.7
 */
ENTROPIC_EXPORT entropic_error_t entropic_deregister_mcp_server(
    entropic_handle_t handle,
    const char* name);

/**
 * @brief List all MCP servers with status information.
 *
 * Returns both in-process and external servers.
 *
 * @param handle Engine handle.
 * @return JSON string: {"servers":{"name":{...ServerInfo...},...}}.
 *         Caller must free with entropic_free(). NULL on error.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.7
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 */
ENTROPIC_EXPORT char* entropic_list_mcp_servers(
    entropic_handle_t handle);

/* ── Storage (v1.8.8) ────────────────────────────────── */

/**
 * @brief Open or create a SQLite storage backend.
 *
 * Initializes persistent storage for conversations, delegations, and
 * session logs. The database file is created if it does not exist.
 * Storage is optional — the engine operates in-memory without it.
 *
 * @param handle Engine handle.
 * @param db_path Path to SQLite database file.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — db_path is NULL.
 *         - ENTROPIC_ERROR_STORAGE_FAILED — database open/init failed.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t entropic_storage_open(
    entropic_handle_t handle,
    const char* db_path);

/**
 * @brief Close the storage backend and flush pending writes.
 *
 * No-op if storage was never opened.
 *
 * @param handle Engine handle.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t entropic_storage_close(
    entropic_handle_t handle);

/* ── Identity (v1.8.9) ───────────────────────────────── */

/**
 * @brief Load an identity by name from the configuration.
 *
 * Resolves the identity name against the loaded config, loads the
 * identity's system prompt and tool filter, and sets it as the
 * active identity for subsequent entropic_run() calls.
 *
 * @param handle Engine handle.
 * @param identity_name Null-terminated identity name (e.g., "eng", "lead").
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — identity_name is NULL.
 *         - ENTROPIC_ERROR_IDENTITY_NOT_FOUND — name not in config.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — identity config malformed.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.9
 */
ENTROPIC_EXPORT entropic_error_t entropic_load_identity(
    entropic_handle_t handle,
    const char* identity_name);

/**
 * @brief Get the current active identity as a JSON string.
 *
 * Returns a JSON object with identity name, system prompt hash,
 * allowed_tools list, and phase configuration.
 *
 * @param handle Engine handle.
 * @param[out] identity_json Output: JSON string. Caller must free with entropic_free().
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — identity_json is NULL.
 *         - ENTROPIC_ERROR_IDENTITY_NOT_FOUND — no identity loaded.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.8.9
 *
 * @par Memory ownership
 * Caller must free *identity_json with entropic_free().
 */
ENTROPIC_EXPORT entropic_error_t entropic_get_identity(
    entropic_handle_t handle,
    char** identity_json);

/* ── Hooks (v1.9.1) ──────────────────────────────────── */

/**
 * @brief Register a callback for an engine hook point.
 *
 * Multiple callbacks can be registered for the same hook point.
 * They execute in ascending priority order (0 = highest priority).
 * A pre-hook returning non-zero cancels the operation.
 * A pre-hook returning modified JSON (via context_json out-param)
 * modifies the operation's input.
 *
 * @param handle     Engine handle.
 * @param hook_point The hook point to register for.
 * @param callback   Function pointer invoked when the hook fires.
 * @param user_data  Opaque pointer passed to callback.
 * @param priority   Execution priority (0 = first, higher = later).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — callback is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — invalid hook_point value.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.1
 */
ENTROPIC_EXPORT entropic_error_t entropic_register_hook(
    entropic_handle_t handle,
    entropic_hook_point_t hook_point,
    entropic_hook_callback_t callback,
    void* user_data,
    int priority);

/**
 * @brief Deregister a previously registered hook callback.
 *
 * Matches on (hook_point, callback, user_data) triple. If no match
 * is found, returns ENTROPIC_OK (idempotent).
 *
 * @param handle     Engine handle.
 * @param hook_point The hook point to deregister from.
 * @param callback   The callback function pointer to remove.
 * @param user_data  The user_data that was passed during registration.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.1
 */
ENTROPIC_EXPORT entropic_error_t entropic_deregister_hook(
    entropic_handle_t handle,
    entropic_hook_point_t hook_point,
    entropic_hook_callback_t callback,
    void* user_data);

#ifdef __cplusplus
}
#endif
