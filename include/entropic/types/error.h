/**
 * @file error.h
 * @brief Error types for cross-.so error reporting.
 *
 * Exceptions do NOT cross .so boundaries. All cross-boundary error
 * reporting uses:
 * 1. Return codes — C API functions return entropic_error_t.
 * 2. Error callbacks — consumer registers for async errors.
 * 3. Error state on handles — entropic_last_error() returns last message.
 *
 * @par Thread safety
 * entropic_last_error() is per-handle, not global. Each handle maintains
 * its own error state. Thread-safe for different handles; concurrent
 * access to the SAME handle requires external synchronization.
 *
 * @version 1.8.0
 */

#pragma once

#include <stddef.h>
#include <entropic/entropic_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Error codes returned by all C API functions.
 *
 * ENTROPIC_OK (0) on success. All other values indicate failure.
 * Use entropic_error_name() to get a human-readable string for logging.
 */
typedef enum {
    ENTROPIC_OK = 0,                          ///< Success
    ENTROPIC_ERROR_INVALID_ARGUMENT,          ///< NULL pointer, empty string, out-of-range value
    ENTROPIC_ERROR_INVALID_CONFIG,            ///< Config validation failed (missing fields, bad values)
    ENTROPIC_ERROR_INVALID_STATE,             ///< Operation not valid in current state (e.g., generate before activate)
    ENTROPIC_ERROR_MODEL_NOT_FOUND,           ///< Model file does not exist at resolved path
    ENTROPIC_ERROR_LOAD_FAILED,               ///< Model load failed (corrupt file, OOM, unsupported format)
    ENTROPIC_ERROR_GENERATE_FAILED,           ///< Generation failed (context overflow, model error)
    ENTROPIC_ERROR_TOOL_NOT_FOUND,            ///< Requested tool not registered
    ENTROPIC_ERROR_PERMISSION_DENIED,         ///< Tool call blocked by permission manager
    ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH,   ///< Plugin API version != engine expected version
    ENTROPIC_ERROR_PLUGIN_LOAD_FAILED,        ///< dlopen/dlsym failed on plugin .so
    ENTROPIC_ERROR_TIMEOUT,                   ///< Operation exceeded time limit
    ENTROPIC_ERROR_CANCELLED,                 ///< Operation cancelled via cancel token
    ENTROPIC_ERROR_OUT_OF_MEMORY,             ///< Allocation failed (system RAM or VRAM)
    ENTROPIC_ERROR_IO,                        ///< File/network I/O error
    ENTROPIC_ERROR_INTERNAL,                  ///< Unexpected internal error (bug)
    ENTROPIC_ERROR_SERVER_ALREADY_EXISTS,     ///< MCP server name already registered (v1.8.7)
    ENTROPIC_ERROR_SERVER_NOT_FOUND,         ///< MCP server name not found (v1.8.7)
    ENTROPIC_ERROR_CONNECTION_FAILED,        ///< Transport connection failed (v1.8.7)
    ENTROPIC_ERROR_INVALID_HANDLE,           ///< NULL or destroyed handle (v1.8.9)
    ENTROPIC_ERROR_TOOL_EXECUTION_FAILED,    ///< Tool call returned error (v1.8.9)
    ENTROPIC_ERROR_STORAGE_FAILED,           ///< Storage operation failed (v1.8.9)
    ENTROPIC_ERROR_IDENTITY_NOT_FOUND,       ///< Identity name not in config (v1.8.9)
    ENTROPIC_ERROR_ALREADY_RUNNING,          ///< entropic_run already in progress on handle (v1.8.9)
    ENTROPIC_ERROR_NOT_RUNNING,              ///< Interrupt called but nothing running (v1.8.9)
    ENTROPIC_ERROR_NOT_IMPLEMENTED,          ///< Feature stub, not yet available (v1.8.9)
    ENTROPIC_ERROR_INTERRUPTED,              ///< Operation interrupted via entropic_interrupt (v1.8.9)
    ENTROPIC_ERROR_ADAPTER_NOT_FOUND,        ///< Adapter name not recognized (v1.9.2)
    ENTROPIC_ERROR_ADAPTER_LOAD_FAILED,      ///< LoRA file invalid or incompatible with base model (v1.9.2)
    ENTROPIC_ERROR_ADAPTER_SWAP_FAILED,      ///< Swap failed (e.g., base model not ACTIVE) (v1.9.2)
    ENTROPIC_ERROR_ADAPTER_CANCELLED,        ///< ON_ADAPTER_SWAP hook cancelled the operation (v1.9.2)
    ENTROPIC_ERROR_GRAMMAR_NOT_FOUND,        ///< Grammar key not in registry (v1.9.3)
    ENTROPIC_ERROR_GRAMMAR_INVALID,          ///< GBNF parse failed (v1.9.3)
    ENTROPIC_ERROR_MCP_KEY_DENIED,           ///< Tool call denied by MCP key set (v1.9.4)
} entropic_error_t;

/**
 * @brief Opaque handle to an entropic engine instance.
 *
 * All per-handle functions (including entropic_last_error) operate on this.
 * Created by entropic_create(), destroyed by entropic_destroy().
 */
typedef struct entropic_engine* entropic_handle_t;

/**
 * @brief Get the last error message for a handle.
 *
 * Returns the error message from the most recent failed operation on this
 * handle. The returned string is owned by the handle and valid until the
 * next API call on the same handle.
 *
 * @param handle Engine handle (may be NULL for global errors during create).
 * @return Null-terminated error message string, or "" if no error.
 *         Owned by the handle — do NOT free.
 * @version 1.8.0
 */
ENTROPIC_EXPORT const char* entropic_last_error(entropic_handle_t handle);

/**
 * @brief Get the human-readable name for an error code.
 *
 * @param code Error code.
 * @return Static string like "ENTROPIC_ERROR_LOAD_FAILED". Never NULL.
 *
 * @par Example
 * @code
 * entropic_error_t err = entropic_configure(h, cfg);
 * if (err != ENTROPIC_OK) {
 *     spdlog::error("configure failed: {} — {}",
 *         entropic_error_name(err), entropic_last_error(h));
 * }
 * @endcode
 * @version 1.8.0
 */
ENTROPIC_EXPORT const char* entropic_error_name(entropic_error_t code);

/**
 * @brief Error callback type for async error reporting.
 *
 * Registered via entropic_set_error_callback(). Called from the thread
 * where the error occurs. The callback MUST NOT call back into the
 * entropic API (deadlock risk). Keep callbacks short — log and return.
 *
 * @param code Error code.
 * @param message Null-terminated error message. Valid only for callback duration.
 * @param user_data Opaque pointer passed during registration.
 * @callback
 * @version 1.8.0
 */
typedef void (*entropic_error_callback_t)(
    entropic_error_t code,
    const char* message,
    void* user_data);

/**
 * @brief Register an error callback on a handle.
 *
 * Only one callback per handle. Setting a new one replaces the previous.
 * Pass NULL to remove.
 *
 * @param handle Engine handle.
 * @param callback Callback function, or NULL to remove.
 * @param user_data Opaque pointer forwarded to callback.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_INVALID_ARGUMENT if handle is NULL.
 * @version 1.8.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_set_error_callback(
    entropic_handle_t handle,
    entropic_error_callback_t callback,
    void* user_data);

#ifdef __cplusplus
}
#endif
