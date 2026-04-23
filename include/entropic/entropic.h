// SPDX-License-Identifier: LGPL-3.0-or-later
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
 * @version 1.9.10
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * @brief Configure engine using layered config resolution.
 *
 * Loads configuration in priority order (highest wins):
 * 1. Compiled defaults
 * 2. Bundled default (data/default_config.yaml)
 * 3. Global config (~/.entropic/config.yaml)
 * 4. Project config ({project_dir}/config.local.yaml)
 * 5. Environment variables (ENTROPIC_*)
 *
 * This is the recommended configuration path for most consumers.
 * The project_dir also sets the working directory for log files
 * and session state (e.g., ".hello-world", ".entropic").
 *
 * @param handle Engine handle.
 * @param project_dir Project config directory (null-terminated).
 *        Contains config.local.yaml and receives session logs.
 *        Pass NULL or "" to use only global + bundled defaults.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — config validation failed.
 *
 * @threadsafety Serialized per-handle.
 * @version 2.0.1
 */
ENTROPIC_EXPORT entropic_error_t entropic_configure_dir(
    entropic_handle_t handle,
    const char* project_dir);

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
 * @brief Set a global stream observer callback.
 *
 * Fires for every token generated from every entry point — interactive
 * entropic_run_streaming(), non-streaming entropic_run(), and delegate
 * child-loop generations. Runs alongside any per-call on_token callback
 * registered via entropic_run_streaming(). Registration before
 * entropic_configure() is allowed — the observer is wired to the engine
 * when it is constructed.
 *
 * At the end of any entropic_run() or entropic_run_streaming() call,
 * the observer is invoked once with (token="", len=0) as a synthetic
 * completion sentinel so consumers can detect end-of-turn regardless
 * of the entry point.
 *
 * Use case: TUI consumers register this once to observe activity from
 * both interactive input and external MCP bridge requests.
 *
 * @threadsafety The observer may be invoked from the engine thread
 *        servicing the current run (serialized per-handle) and from
 *        the external-bridge worker thread that drives async MCP
 *        ask tasks. Consumer callbacks must be thread-safe.
 *
 * @param handle Engine handle.
 * @param observer Token callback (same signature as on_token). NULL to clear.
 * @param user_data Forwarded to observer.
 * @return ENTROPIC_OK on success.
 * @version 2.0.6-rc16
 */
ENTROPIC_EXPORT entropic_error_t entropic_set_stream_observer(
    entropic_handle_t handle,
    void (*observer)(const char* token, size_t len, void* user_data),
    void* user_data);

/**
 * @brief Register an engine state-change observer.
 *
 * Fires on every AgentState transition. State integers map 1:1 to
 * entropic_agent_state_t. Used by async subscribers (e.g. the external
 * MCP bridge) to project engine-side phases — PLANNING, EXECUTING,
 * WAITING_TOOL, VERIFYING, DELEGATING — onto user-visible task status.
 * Pass observer=NULL to clear.
 *
 * @threadsafety Callback may fire from the engine thread or a
 *        child-loop delegation thread. Must be thread-safe.
 *
 * @param handle Engine handle.
 * @param observer State-change callback (state_int, user_data).
 * @param user_data Forwarded to observer.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *
 * @version 2.0.6-rc16.2
 */
ENTROPIC_EXPORT entropic_error_t entropic_set_state_observer(
    entropic_handle_t handle,
    void (*observer)(int state, void* user_data),
    void* user_data);

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

/* ── Conversation Context (v2.0.1) ───────────────────── */

/**
 * @brief Clear conversation history, starting a new session.
 *
 * Removes all messages (system, user, assistant, tool) from the
 * conversation. The next entropic_run() call starts fresh with
 * only the system prompt.
 *
 * @param handle Engine handle.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *
 * @threadsafety Serialized per-handle.
 * @version 2.0.1
 */
ENTROPIC_EXPORT entropic_error_t entropic_context_clear(
    entropic_handle_t handle);

/**
 * @brief Get the current conversation history as a JSON array.
 *
 * Returns the full message history including system prompt, user
 * messages, assistant responses, and tool call results.
 *
 * @param handle Engine handle.
 * @param[out] messages_json Output: JSON array of message objects.
 *             Each object has "role" and "content" fields.
 *             Caller must free with entropic_free().
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — messages_json is NULL.
 *
 * @threadsafety Serialized per-handle.
 * @version 2.0.1
 *
 * @par Memory ownership
 * Caller must free *messages_json with entropic_free().
 */
ENTROPIC_EXPORT entropic_error_t entropic_context_get(
    entropic_handle_t handle,
    char** messages_json);

/**
 * @brief Get the number of messages in the conversation.
 *
 * @param handle Engine handle.
 * @param[out] count Output: number of messages.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — count is NULL.
 *
 * @threadsafety Thread-safe (read-only).
 * @version 2.0.1
 */
ENTROPIC_EXPORT entropic_error_t entropic_context_count(
    entropic_handle_t handle,
    size_t* count);

/**
 * @brief Get loop metrics from the most recent run as JSON.
 *
 * Populates *out with a malloc'd JSON string containing flat fields
 * (iterations, tool_calls, tokens_used, errors, duration_ms) and a
 * per_tier object keyed by tier name. Caller must entropic_free(*out).
 *
 * @param handle Engine handle.
 * @param[out] out Output: malloc'd JSON string. Caller frees via entropic_free().
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — out is NULL.
 *
 * @threadsafety Thread-safe (read-only).
 * @version 2.0.6-rc16.2
 */
ENTROPIC_EXPORT entropic_error_t entropic_metrics_json(
    entropic_handle_t handle,
    char** out);

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

/* ── LoRA Adapters (v1.9.2) ───────────────────────────── */

/**
 * @brief Load a LoRA adapter into RAM.
 *
 * @param handle Engine handle.
 * @param adapter_name Unique adapter identifier.
 * @param adapter_path Path to the LoRA .gguf file.
 * @param base_model_path Base model this adapter targets (must be loaded).
 * @param scale LoRA scaling factor (1.0 = full strength).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_MODEL_NOT_FOUND — base model not loaded.
 *         - ENTROPIC_ERROR_ADAPTER_LOAD_FAILED — adapter file invalid.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — adapter already loaded.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.2
 */
ENTROPIC_EXPORT entropic_error_t entropic_adapter_load(
    entropic_handle_t handle,
    const char* adapter_name,
    const char* adapter_path,
    const char* base_model_path,
    float scale);

/**
 * @brief Unload a LoRA adapter.
 *
 * @param handle Engine handle.
 * @param adapter_name Adapter to unload.
 * @return ENTROPIC_OK on success. Idempotent if already COLD.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.2
 */
ENTROPIC_EXPORT entropic_error_t entropic_adapter_unload(
    entropic_handle_t handle,
    const char* adapter_name);

/**
 * @brief Swap the active LoRA adapter.
 *
 * @param handle Engine handle.
 * @param adapter_name Adapter to activate (must be WARM or HOT).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_ADAPTER_NOT_FOUND — adapter not loaded.
 *         - ENTROPIC_ERROR_ADAPTER_SWAP_FAILED — swap failed.
 *         - ENTROPIC_ERROR_ADAPTER_CANCELLED — hook cancelled.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.2
 */
ENTROPIC_EXPORT entropic_error_t entropic_adapter_swap(
    entropic_handle_t handle,
    const char* adapter_name);

/**
 * @brief Query adapter state.
 *
 * @param handle Engine handle.
 * @param adapter_name Adapter identifier.
 * @return State as int: 0=COLD, 1=WARM, 2=HOT. -1 if not found.
 *
 * @threadsafety Thread-safe.
 * @version 1.9.2
 */
ENTROPIC_EXPORT int entropic_adapter_state(
    entropic_handle_t handle,
    const char* adapter_name);

/**
 * @brief Get adapter info as JSON.
 *
 * @param handle Engine handle.
 * @param adapter_name Adapter identifier.
 * @return JSON string with adapter metadata. Caller frees with
 *         entropic_free(). NULL if adapter not found.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.2
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 */
ENTROPIC_EXPORT char* entropic_adapter_info(
    entropic_handle_t handle,
    const char* adapter_name);

/**
 * @brief List all adapters as JSON array.
 *
 * @param handle Engine handle.
 * @return JSON array of AdapterInfo objects. Caller frees with
 *         entropic_free().
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.2
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 */
ENTROPIC_EXPORT char* entropic_adapter_list(entropic_handle_t handle);

/* ── Grammar Registry (v1.9.3) ───────────────────────── */

/**
 * @brief Register a grammar by key from a GBNF content string.
 *
 * @param handle Engine handle.
 * @param key Unique grammar name (null-terminated).
 * @param gbnf_content Raw GBNF grammar string (null-terminated).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — key or gbnf_content is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — key already exists.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.3
 */
ENTROPIC_EXPORT entropic_error_t entropic_grammar_register(
    entropic_handle_t handle,
    const char* key,
    const char* gbnf_content);

/**
 * @brief Register a grammar from a file path.
 *
 * @param handle Engine handle.
 * @param key Grammar name (if NULL, uses filename stem).
 * @param path Path to .gbnf file (null-terminated).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — path is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — key already exists.
 *         - ENTROPIC_ERROR_LOAD_FAILED — file unreadable.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.3
 */
ENTROPIC_EXPORT entropic_error_t entropic_grammar_register_file(
    entropic_handle_t handle,
    const char* key,
    const char* path);

/**
 * @brief Remove a grammar from the registry.
 *
 * @param handle Engine handle.
 * @param key Grammar name (null-terminated).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — key is NULL.
 *         - ENTROPIC_ERROR_GRAMMAR_NOT_FOUND — key not registered.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.3
 */
ENTROPIC_EXPORT entropic_error_t entropic_grammar_deregister(
    entropic_handle_t handle,
    const char* key);

/**
 * @brief Get grammar content by key.
 *
 * @param handle Engine handle.
 * @param key Grammar name (null-terminated).
 * @return GBNF content string. Caller frees with entropic_free().
 *         NULL if key not found.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.3
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 */
ENTROPIC_EXPORT char* entropic_grammar_get(
    entropic_handle_t handle,
    const char* key);

/**
 * @brief Validate a GBNF grammar string without registering.
 *
 * @param gbnf_content Raw GBNF string to validate (null-terminated).
 * @return NULL on success (valid grammar). Error description string on
 *         failure. Caller frees with entropic_free().
 *
 * @note This is a static function — does not require an engine handle.
 *       Can be called before engine initialization.
 *
 * @threadsafety Thread-safe.
 * @version 1.9.3
 *
 * @par Memory ownership
 * Caller must free non-NULL returned string with entropic_free().
 */
ENTROPIC_EXPORT char* entropic_grammar_validate(const char* gbnf_content);

/**
 * @brief List all registered grammars as JSON array.
 *
 * @param handle Engine handle.
 * @return JSON array of grammar metadata objects:
 *         [{"key":"compactor","source":"bundled","validated":true}, ...]
 *         Content strings are NOT included (use entropic_grammar_get).
 *         Caller frees with entropic_free().
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.3
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 */
ENTROPIC_EXPORT char* entropic_grammar_list(entropic_handle_t handle);

/* ── GPU Resource Profiles (v1.9.7) ──────────────────── */

/**
 * @brief Register a custom GPU resource profile.
 *
 * @param handle Engine handle.
 * @param profile_json JSON string: {"name":"...", "n_batch":N,
 *        "n_threads":N, "n_threads_batch":N, "description":"..."}.
 *        Only "name" is required; other fields default.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — profile_json is NULL.
 *         - ENTROPIC_ERROR_ALREADY_EXISTS — name already registered.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.7
 */
ENTROPIC_EXPORT entropic_error_t entropic_profile_register(
    entropic_handle_t handle,
    const char* profile_json);

/**
 * @brief Remove a GPU resource profile.
 *
 * @param handle Engine handle.
 * @param name Profile name (null-terminated).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_PROFILE_NOT_FOUND — name not registered.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.7
 */
ENTROPIC_EXPORT entropic_error_t entropic_profile_deregister(
    entropic_handle_t handle,
    const char* name);

/**
 * @brief Get a profile by name as JSON.
 *
 * @param handle Engine handle.
 * @param name Profile name (null-terminated).
 * @return JSON string of GPUResourceProfile fields.
 *         NULL if handle is NULL or name not found.
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.7
 */
ENTROPIC_EXPORT char* entropic_profile_get(
    entropic_handle_t handle,
    const char* name);

/**
 * @brief List all registered profiles as JSON array.
 *
 * @param handle Engine handle.
 * @return JSON array of profile objects.
 *         NULL if handle is NULL.
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.7
 */
ENTROPIC_EXPORT char* entropic_profile_list(entropic_handle_t handle);

/* ── Throughput Query (v1.9.7) ───────────────────────── */

/**
 * @brief Get current throughput estimate for a model.
 *
 * @param handle Engine handle.
 * @param model_path Path to the model (same key as ModelConfig.path).
 * @return Tokens per second (EWMA). 0.0 if no data or handle is NULL.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.7
 */
ENTROPIC_EXPORT double entropic_throughput_tok_per_sec(
    entropic_handle_t handle,
    const char* model_path);

/**
 * @brief Reset throughput tracking data for a model.
 *
 * @param handle Engine handle.
 * @param model_path Model path. NULL = reset all models.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.7
 */
ENTROPIC_EXPORT void entropic_throughput_reset(
    entropic_handle_t handle,
    const char* model_path);

/* ── MCP Authorization (v1.9.4) ──────────────────────── */

/**
 * @brief Access level enum for MCP authorization.
 * @version 1.9.4
 */
typedef enum {
    ENTROPIC_MCP_ACCESS_NONE  = 0, ///< No access
    ENTROPIC_MCP_ACCESS_READ  = 1, ///< Read-only operations
    ENTROPIC_MCP_ACCESS_WRITE = 2, ///< Read + write operations
} entropic_mcp_access_level_t;

/**
 * @brief Grant an MCP tool key to an identity.
 *
 * If the identity has no key set, one is created automatically
 * (enabling enforcement).
 *
 * @param handle Engine handle.
 * @param identity_name Identity/tier name (null-terminated).
 * @param pattern Tool pattern string (null-terminated).
 * @param level Access level to grant.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — identity_name or pattern is NULL.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.4
 */
ENTROPIC_EXPORT entropic_error_t entropic_grant_mcp_key(
    entropic_handle_t handle,
    const char* identity_name,
    const char* pattern,
    entropic_mcp_access_level_t level);

/**
 * @brief Revoke an MCP tool key from an identity.
 *
 * @param handle Engine handle.
 * @param identity_name Identity/tier name (null-terminated).
 * @param pattern Tool pattern string (null-terminated).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — identity_name or pattern is NULL.
 *         - ENTROPIC_ERROR_IDENTITY_NOT_FOUND — identity not registered.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.4
 */
ENTROPIC_EXPORT entropic_error_t entropic_revoke_mcp_key(
    entropic_handle_t handle,
    const char* identity_name,
    const char* pattern);

/**
 * @brief Check if a tool call is authorized for an identity.
 *
 * @param handle Engine handle.
 * @param identity_name Identity/tier name (null-terminated).
 * @param tool_name Fully-qualified tool name (null-terminated).
 * @param level Required access level.
 * @return 1 if authorized, 0 if denied, -1 on error.
 *
 * @threadsafety Thread-safe.
 * @version 1.9.4
 */
ENTROPIC_EXPORT int entropic_check_mcp_key(
    entropic_handle_t handle,
    const char* identity_name,
    const char* tool_name,
    entropic_mcp_access_level_t level);

/**
 * @brief List all MCP keys for an identity as JSON.
 *
 * @param handle Engine handle.
 * @param identity_name Identity/tier name (null-terminated).
 * @return JSON array of key objects. Caller frees with entropic_free().
 *         NULL if identity not found or error.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.4
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 */
ENTROPIC_EXPORT char* entropic_list_mcp_keys(
    entropic_handle_t handle,
    const char* identity_name);

/**
 * @brief Grant a key from one identity to another.
 *
 * The granter must possess the key at >= the granted level.
 *
 * @param handle Engine handle.
 * @param granter Granting identity (null-terminated).
 * @param grantee Receiving identity (null-terminated).
 * @param pattern Tool pattern to grant (null-terminated).
 * @param level Access level to grant.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_IDENTITY_NOT_FOUND — either identity not registered.
 *         - ENTROPIC_ERROR_PERMISSION_DENIED — granter lacks the key.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.4
 */
ENTROPIC_EXPORT entropic_error_t entropic_grant_mcp_key_from(
    entropic_handle_t handle,
    const char* granter,
    const char* grantee,
    const char* pattern,
    entropic_mcp_access_level_t level);

/**
 * @brief Serialize all identity key sets to JSON.
 *
 * @param handle Engine handle.
 * @return JSON object string. Caller frees with entropic_free().
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.4
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 */
ENTROPIC_EXPORT char* entropic_serialize_mcp_keys(
    entropic_handle_t handle);

/**
 * @brief Deserialize all identity key sets from JSON.
 *
 * Replaces all current key sets with the deserialized state.
 *
 * @param handle Engine handle.
 * @param json JSON object string.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — json is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — parse failure.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.4
 */
ENTROPIC_EXPORT entropic_error_t entropic_deserialize_mcp_keys(
    entropic_handle_t handle,
    const char* json);

/* ── Audit (v1.9.5) ──────────────────────────────────────── */

/**
 * @brief Flush the audit logger to disk immediately.
 * @param handle Engine handle.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — no audit logger configured.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.5
 */
ENTROPIC_EXPORT entropic_error_t entropic_audit_flush(
    entropic_handle_t handle);

/**
 * @brief Get the number of audit log entries recorded this session.
 * @param handle Engine handle.
 * @param[out] count Output: number of entries recorded.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — count is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — no audit logger configured.
 *
 * @threadsafety Lock-free read.
 * @version 1.9.5
 */
ENTROPIC_EXPORT entropic_error_t entropic_audit_count(
    entropic_handle_t handle,
    size_t* count);

/**
 * @brief Read audit log entries from a JSONL file.
 *
 * Reads and parses entries from an audit.jsonl file. No tool calls
 * are executed — this is inspection only.
 *
 * @param handle Engine handle.
 * @param path Path to audit.jsonl file (null-terminated).
 * @param filter_json Filter criteria as JSON, or NULL for no filter.
 *        Format: {"caller_id": "eng", "tool_name": "filesystem.*"}
 *        All fields optional. Absent fields match everything.
 * @param[out] result_json Output: JSON array of audit entries.
 *             Caller must free with entropic_free().
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — path or result_json is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — path cannot be read.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.5
 */
ENTROPIC_EXPORT entropic_error_t entropic_audit_read(
    entropic_handle_t handle,
    const char* path,
    const char* filter_json,
    char** result_json);

/* ── Dynamic Identity Management (v1.9.6) ────────────── */

/**
 * @brief Create a dynamic identity.
 *
 * @param handle Engine handle.
 * @param config_json Identity configuration as JSON string.
 *        Required fields: "name", "system_prompt", "focus" (array, min 1).
 *        Optional fields: "examples", "grammar_id", "auto_chain",
 *        "allowed_tools", "bash_commands", "mcp_keys", "adapter_path",
 *        "max_output_tokens", "temperature", "repeat_penalty",
 *        "enable_thinking", "model_preference", "interstitial",
 *        "routable", "role_type", "explicit_completion", "phases".
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — config_json is NULL.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — validation failed.
 *         - ENTROPIC_ERROR_LIMIT_REACHED — max_identities exceeded.
 *         - ENTROPIC_ERROR_ALREADY_EXISTS — name already taken.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.6
 */
ENTROPIC_EXPORT entropic_error_t entropic_create_identity(
    entropic_handle_t handle,
    const char* config_json);

/**
 * @brief Update a dynamic identity.
 *
 * @param handle Engine handle.
 * @param name Identity name (null-terminated).
 * @param config_json Full replacement config as JSON string.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — name or config_json is NULL.
 *         - ENTROPIC_ERROR_IDENTITY_NOT_FOUND — identity doesn't exist.
 *         - ENTROPIC_ERROR_PERMISSION_DENIED — identity is static.
 *         - ENTROPIC_ERROR_INVALID_CONFIG — validation failed.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.6
 */
ENTROPIC_EXPORT entropic_error_t entropic_update_identity(
    entropic_handle_t handle,
    const char* name,
    const char* config_json);

/**
 * @brief Destroy a dynamic identity.
 *
 * @param handle Engine handle.
 * @param name Identity name (null-terminated).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — name is NULL.
 *         - ENTROPIC_ERROR_IDENTITY_NOT_FOUND — identity doesn't exist.
 *         - ENTROPIC_ERROR_PERMISSION_DENIED — identity is static.
 *         - ENTROPIC_ERROR_IN_USE — identity active in delegation.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.6
 */
ENTROPIC_EXPORT entropic_error_t entropic_destroy_identity(
    entropic_handle_t handle,
    const char* name);

/**
 * @brief Get identity configuration as JSON by name.
 *
 * @param handle Engine handle.
 * @param name Identity name (null-terminated).
 * @return JSON string of identity config. Caller frees with entropic_free().
 *         NULL if identity not found or handle is NULL.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.6
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 */
ENTROPIC_EXPORT char* entropic_get_identity_config(
    entropic_handle_t handle,
    const char* name);

/**
 * @brief List all identity names as JSON array.
 *
 * @param handle Engine handle.
 * @return JSON array of identity name strings.
 *         Caller frees with entropic_free(). NULL on error.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.6
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 */
ENTROPIC_EXPORT char* entropic_list_identities(
    entropic_handle_t handle);

/**
 * @brief Get identity count.
 *
 * @param handle Engine handle.
 * @param total Output: total identity count (static + dynamic).
 * @param dynamic Output: dynamic identity count only. Pass NULL to skip.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — total is NULL.
 *
 * @threadsafety Thread-safe.
 * @version 1.9.6
 */
ENTROPIC_EXPORT entropic_error_t entropic_identity_count(
    entropic_handle_t handle,
    size_t* total,
    size_t* dynamic);

/* ── Compaction Hooks (v1.9.9) ───────────────────────── */

/**
 * @brief Compactor function type.
 *
 * A compactor takes a JSON array of messages, a configuration JSON,
 * and produces a compacted JSON array of messages plus a summary string.
 *
 * @param messages_json   JSON array of messages to compact.
 * @param config_json     Compaction configuration (threshold, identity, etc.).
 * @param out_messages    Output: compacted messages JSON. Caller frees with entropic_free().
 * @param out_summary     Output: summary text. Caller frees with entropic_free(). May be NULL.
 * @param user_data       Opaque pointer from registration.
 * @return 0 on success, non-zero on failure.
 *
 * @par Config JSON schema:
 * @code
 * {
 *   "identity": "eng",
 *   "token_count": 12000,
 *   "max_tokens": 16384,
 *   "threshold_percent": 0.75,
 *   "force": false
 * }
 * @endcode
 *
 * @callback
 * @version 1.9.10
 */
typedef int (*entropic_compactor_fn)(
    const char* messages_json,
    const char* config_json,
    char** out_messages,
    char** out_summary,
    void* user_data);

/**
 * @brief Trigger compaction on the current context.
 *
 * Forces compaction regardless of threshold. Runs through the full
 * pipeline: PRE_COMPACT hooks -> compactor -> POST_COMPACT hooks.
 *
 * @param handle        Engine handle.
 * @param identity      Identity context for compactor selection (NULL = current).
 * @param result_json   Output: compaction result JSON. Caller frees with entropic_free().
 *                      NULL if no compaction occurred (e.g., cancelled by hook).
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE
 *         - ENTROPIC_ERROR_COMPACTION_FAILED
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.9
 */
ENTROPIC_EXPORT entropic_error_t entropic_compact(
    entropic_handle_t handle,
    const char* identity,
    char** result_json);

/**
 * @brief Register a custom compactor for an identity.
 *
 * Replaces the default compaction strategy for the specified identity.
 * Pass identity="" to set a global fallback for all identities without
 * a per-identity compactor.
 *
 * Resolution order: per-identity -> global custom ("") -> built-in default.
 * Only one compactor per identity. Re-registering replaces the previous.
 *
 * @param handle     Engine handle.
 * @param identity   Identity name, or "" for global fallback.
 * @param compactor  Compactor function pointer.
 * @param user_data  Opaque pointer passed to compactor on each call.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE
 *         - ENTROPIC_ERROR_INVALID_CONFIG (NULL compactor)
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.9
 */
ENTROPIC_EXPORT entropic_error_t entropic_register_compactor(
    entropic_handle_t handle,
    const char* identity,
    entropic_compactor_fn compactor,
    void* user_data);

/**
 * @brief Deregister a custom compactor for an identity.
 *
 * After deregistration, the identity falls back to the global custom
 * compactor (if any), then to the built-in default.
 *
 * @param handle     Engine handle.
 * @param identity   Identity name, or "" for global fallback.
 * @return ENTROPIC_OK on success (idempotent).
 *         - ENTROPIC_ERROR_INVALID_HANDLE
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.9
 */
ENTROPIC_EXPORT entropic_error_t entropic_deregister_compactor(
    entropic_handle_t handle,
    const char* identity);

/**
 * @brief Get the built-in default compactor function pointer.
 *
 * Useful for consumers that want to wrap the default behavior
 * (e.g., add logging, metrics, or post-processing around it).
 *
 * @param handle     Engine handle.
 * @param compactor  Output: default compactor function pointer.
 * @param user_data  Output: user_data to pass to the default compactor.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT (NULL compactor output)
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.9
 */
ENTROPIC_EXPORT entropic_error_t entropic_get_default_compactor(
    entropic_handle_t handle,
    entropic_compactor_fn* compactor,
    void** user_data);

/* ── Log-Probability Evaluation (v1.9.10) ────────────── */

/**
 * @brief Per-token log-probability result.
 *
 * Contains per-token log-probabilities for a sequence evaluated
 * against a loaded model. The logprobs array has (n_tokens - 1)
 * entries: logprobs[i] is the log-probability of tokens[i+1]
 * given tokens[0..i].
 *
 * The struct itself is caller-owned (stack or heap). The internal
 * logprobs and tokens arrays are engine-allocated and must be freed
 * via entropic_free_logprob_result().
 *
 * @version 1.9.10
 */
typedef struct entropic_logprob_result {
    float* logprobs;        /**< Per-token log-probabilities (N-1 values). Engine-allocated. */
    int32_t* tokens;        /**< Input tokens echoed back (N values). Engine-allocated. */
    float perplexity;       /**< exp(-mean(logprobs)) over the sequence. */
    float total_logprob;    /**< Sum of all logprob values. */
    int n_tokens;           /**< Number of input tokens. */
    int n_logprobs;         /**< Number of logprob values (n_tokens - 1). */
} entropic_logprob_result_t;

/**
 * @brief Evaluate per-token log-probabilities for a token sequence.
 *
 * Runs the token sequence through the model and returns the
 * log-probability of each token given its preceding context.
 * Evaluation-only — no sampling, no generation, no KV cache
 * mutation visible to ongoing generation.
 *
 * @param handle     Engine handle.
 * @param model_id   Model identifier (tier name or model key).
 * @param tokens     Array of token IDs to evaluate.
 * @param n_tokens   Number of tokens (minimum 2).
 * @param result     Output: logprob result. Caller frees internal
 *                   arrays with entropic_free_logprob_result().
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE
 *         - ENTROPIC_ERROR_MODEL_NOT_FOUND (unknown model_id)
 *         - ENTROPIC_ERROR_MODEL_NOT_ACTIVE (model not ACTIVE)
 *         - ENTROPIC_ERROR_EVAL_CONTEXT_FULL (exceeds n_ctx)
 *         - ENTROPIC_ERROR_INVALID_CONFIG (n_tokens < 2)
 *         - ENTROPIC_ERROR_EVAL_FAILED (llama_decode error)
 *
 * @threadsafety Serialized per-model via eval_mutex. Does not
 *               block generation on the same model.
 * @version 1.9.10
 */
ENTROPIC_EXPORT entropic_error_t entropic_get_logprobs(
    entropic_handle_t handle,
    const char* model_id,
    const int32_t* tokens,
    int n_tokens,
    entropic_logprob_result_t* result);

/**
 * @brief Compute perplexity for a token sequence.
 *
 * Convenience — calls entropic_get_logprobs() internally and
 * returns only the perplexity value.
 *
 * @param handle     Engine handle.
 * @param model_id   Model identifier (tier name or model key).
 * @param tokens     Array of token IDs to evaluate.
 * @param n_tokens   Number of tokens (minimum 2).
 * @param perplexity Output: perplexity value.
 * @return ENTROPIC_OK on success. Same error codes as
 *         entropic_get_logprobs().
 *
 * @threadsafety Same as entropic_get_logprobs().
 * @version 1.9.10
 */
ENTROPIC_EXPORT entropic_error_t entropic_compute_perplexity(
    entropic_handle_t handle,
    const char* model_id,
    const int32_t* tokens,
    int n_tokens,
    float* perplexity);

/**
 * @brief Free internal arrays of a logprob result.
 *
 * Frees the logprobs and tokens arrays, then NULLs the pointers
 * to prevent double-free. The result struct itself is caller-owned
 * and is NOT freed.
 *
 * @param result Pointer to result struct. NULL-safe (no-op).
 * @version 1.9.10
 */
ENTROPIC_EXPORT void entropic_free_logprob_result(
    entropic_logprob_result_t* result);

/* ── Constitutional Validation (v1.9.8) ──────────────── */

/**
 * @brief Enable or disable constitutional validation.
 *
 * When disabled, the POST_GENERATE hook is deregistered (zero
 * overhead). When re-enabled, it is re-registered.
 *
 * @param handle Engine handle.
 * @param enabled true to enable, false to disable.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.8
 */
ENTROPIC_EXPORT entropic_error_t entropic_validation_set_enabled(
    entropic_handle_t handle,
    bool enabled);

/**
 * @brief Set per-identity validation override.
 *
 * @param handle Engine handle.
 * @param identity_name Identity name (null-terminated).
 * @param enabled true to enable, false to disable for this identity.
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *         - ENTROPIC_ERROR_INVALID_ARGUMENT — identity_name is NULL.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.8
 */
ENTROPIC_EXPORT entropic_error_t entropic_validation_set_identity(
    entropic_handle_t handle,
    const char* identity_name,
    bool enabled);

/**
 * @brief Get the last validation result as JSON.
 *
 * @param handle Engine handle.
 * @return JSON string with validation metadata from the last generation.
 *         NULL if no validation has run. Caller frees with entropic_free().
 *
 *         Return format:
 *         {"was_revised": false, "revision_count": 0, "compliant": true,
 *          "violations": [], "tier": "eng"}
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.8
 */
ENTROPIC_EXPORT char* entropic_validation_last_result(
    entropic_handle_t handle);

/* ── Vision / Multimodal (v1.9.11) ────────────────────── */

/**
 * @brief Check if a model has vision capability.
 *
 * Returns 1 when the specified model has an mmproj loaded (vision
 * encoder active). Returns 0 for text-only models or unknown model_id.
 *
 * @param handle Engine handle.
 * @param model_id Model identifier (tier name, e.g. "primary").
 * @return 1 if vision-capable (mmproj loaded), 0 if text-only.
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.11
 */
ENTROPIC_EXPORT int entropic_model_has_vision(
    entropic_handle_t handle,
    const char* model_id);

/* ── Diagnose / Self-Inspection (v1.9.12) ─────────────── */

/**
 * @brief Get the diagnostic prompt text for /diagnose command.
 *
 * Returns the bundled diagnostic prompt that consumers inject as
 * a user message to trigger model self-diagnosis. The prompt
 * instructs the model to call entropic.diagnose and produce
 * structured self-assessment.
 *
 * @param handle Engine handle.
 * @param[out] prompt_out Output: diagnostic prompt string.
 *             Caller must free with entropic_free().
 * @return ENTROPIC_OK on success.
 *         - ENTROPIC_ERROR_INVALID_HANDLE — handle is NULL.
 *
 * @par Memory ownership
 * Caller must free returned string with entropic_free().
 *
 * @threadsafety Serialized per-handle.
 * @version 1.9.12
 */
ENTROPIC_EXPORT entropic_error_t entropic_get_diagnostic_prompt(
    entropic_handle_t handle,
    char** prompt_out);

#ifdef __cplusplus
}
#endif
