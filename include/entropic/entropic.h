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
 * A single handle is NOT thread-safe. Use one handle per thread, or
 * synchronize externally. Different handles are fully independent.
 *
 * @par Error handling
 * All functions return entropic_error_t. On failure, call
 * entropic_last_error(handle) for the detailed message.
 *
 * @par Memory ownership
 * Functions returning char* transfer ownership to the caller.
 * Free with entropic_free(). Consumers providing buffers the engine
 * will free (e.g., hook modified_json) MUST allocate with
 * entropic_alloc(). Strings returned as const char* are owned by
 * the handle and valid until the next call on that handle.
 *
 * @version 1.8.0
 */

#pragma once

#include <entropic/entropic_config.h>
#include <entropic/entropic_export.h>
#include <entropic/types/error.h>

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
 * @param[out] handle Pointer to receive the new handle.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_OUT_OF_MEMORY on failure.
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
 * @return ENTROPIC_OK, ENTROPIC_ERROR_INVALID_CONFIG, ENTROPIC_ERROR_INVALID_ARGUMENT.
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
 * Internally calls entropic_configure() after conversion.
 *
 * @param handle Engine handle.
 * @param config_path Path to YAML (.yaml/.yml) or JSON (.json) config file.
 * @return ENTROPIC_OK, ENTROPIC_ERROR_INVALID_CONFIG, ENTROPIC_ERROR_INVALID_ARGUMENT.
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
 * @param handle Engine handle to destroy.
 * @version 1.8.0
 */
ENTROPIC_EXPORT void entropic_destroy(entropic_handle_t handle);

/* ── Version ──────────────────────────────────────────── */

/**
 * @brief Get the library version string.
 * @return Static string like "1.8.0". Never NULL. Do NOT free.
 * @version 1.8.0
 */
ENTROPIC_EXPORT const char* entropic_version(void);

/**
 * @brief Get the plugin API version number.
 *
 * Plugins call this to verify compatibility before registering.
 *
 * @return Integer API version (incremented on breaking changes).
 * @version 1.8.0
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
 * @version 1.8.0
 */
ENTROPIC_EXPORT void entropic_free(void* ptr);

/* ── Generation (stubs — implemented in v1.8.4) ──────── */

/**
 * @brief Single-turn blocking generation.
 *
 * @param handle Engine handle.
 * @param input User message (null-terminated).
 * @param[out] result_json JSON result string. Caller must free with entropic_free().
 * @return ENTROPIC_OK, ENTROPIC_ERROR_GENERATE_FAILED, ENTROPIC_ERROR_INVALID_STATE.
 *
 * @note Stub in v1.8.0. Returns ENTROPIC_ERROR_INVALID_STATE until v1.8.4.
 * @version 1.8.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_run(
    entropic_handle_t handle,
    const char* input,
    char** result_json);

/**
 * @brief Streaming generation with token callback.
 *
 * @param handle Engine handle.
 * @param input User message.
 * @param on_token Called for each generated token. Must be thread-safe.
 * @param user_data Forwarded to on_token.
 * @param cancel_flag Pointer to int. Set to non-zero to cancel. May be NULL.
 * @return ENTROPIC_OK, ENTROPIC_ERROR_GENERATE_FAILED, ENTROPIC_ERROR_CANCELLED.
 *
 * @note Stub in v1.8.0. Returns ENTROPIC_ERROR_INVALID_STATE until v1.8.4.
 * @version 1.8.0
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
 * Thread-safe. Can be called from any thread while entropic_run() or
 * entropic_run_streaming() is executing on another thread.
 *
 * @param handle Engine handle.
 * @return ENTROPIC_OK, ENTROPIC_ERROR_INVALID_STATE (nothing running).
 *
 * @note Stub in v1.8.0.
 * @version 1.8.0
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
 * @return ENTROPIC_OK, ENTROPIC_ERROR_SERVER_ALREADY_EXISTS,
 *         ENTROPIC_ERROR_CONNECTION_FAILED.
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
 * @return ENTROPIC_OK, ENTROPIC_ERROR_SERVER_NOT_FOUND.
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
 * @version 1.8.7
 */
ENTROPIC_EXPORT char* entropic_list_mcp_servers(
    entropic_handle_t handle);

#ifdef __cplusplus
}
#endif
