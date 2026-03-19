/**
 * @file error.cpp
 * @brief Error type implementations for librentropic-types.
 * @version 1.8.0
 */

#include <entropic/types/error.h>

/// @brief Thread-local error message for pre-handle errors (during create).
static thread_local char s_global_error[512] = "";

/**
 * @brief Get the last error message for a handle.
 * @param handle Engine handle (may be NULL for pre-creation errors).
 * @return Null-terminated error message, or "" if no error.
 * @version 1.8.0
 */
extern "C" const char* entropic_last_error(entropic_handle_t handle) {
    // TODO(v1.8.4): Look up per-handle error state.
    // Pre-handle errors (NULL handle) use thread-local global.
    (void)handle;
    return s_global_error;
}

/**
 * @brief Get the human-readable name for an error code.
 * @param code Error code.
 * @return Static string. Never NULL.
 * @version 1.8.0
 */
/// @brief Lookup table: error code → name string.
static const char* const s_error_names[] = {
    "ENTROPIC_OK",                             // 0
    "ENTROPIC_ERROR_INVALID_ARGUMENT",         // 1
    "ENTROPIC_ERROR_INVALID_CONFIG",           // 2
    "ENTROPIC_ERROR_INVALID_STATE",            // 3
    "ENTROPIC_ERROR_MODEL_NOT_FOUND",          // 4
    "ENTROPIC_ERROR_LOAD_FAILED",              // 5
    "ENTROPIC_ERROR_GENERATE_FAILED",          // 6
    "ENTROPIC_ERROR_TOOL_NOT_FOUND",           // 7
    "ENTROPIC_ERROR_PERMISSION_DENIED",        // 8
    "ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH",  // 9
    "ENTROPIC_ERROR_PLUGIN_LOAD_FAILED",       // 10
    "ENTROPIC_ERROR_TIMEOUT",                  // 11
    "ENTROPIC_ERROR_CANCELLED",                // 12
    "ENTROPIC_ERROR_OUT_OF_MEMORY",            // 13
    "ENTROPIC_ERROR_IO",                       // 14
    "ENTROPIC_ERROR_INTERNAL",                 // 15
};

static constexpr int s_error_count =
    static_cast<int>(sizeof(s_error_names) / sizeof(s_error_names[0]));

extern "C" const char* entropic_error_name(entropic_error_t code) {
    int idx = static_cast<int>(code);
    return (idx >= 0 && idx < s_error_count) ? s_error_names[idx] : "ENTROPIC_ERROR_UNKNOWN";
}

/**
 * @brief Register an error callback on a handle.
 * @param handle Engine handle.
 * @param callback Callback function, or NULL to remove.
 * @param user_data Opaque pointer forwarded to callback.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_INVALID_ARGUMENT if handle is NULL.
 * @version 1.8.0
 */
extern "C" entropic_error_t entropic_set_error_callback(
    entropic_handle_t handle,
    entropic_error_callback_t callback,
    void* user_data) {
    // TODO(v1.8.4): Store callback on per-handle state.
    (void)callback;
    (void)user_data;
    if (handle == nullptr) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    return ENTROPIC_OK;
}
