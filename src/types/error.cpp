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
extern "C" const char* entropic_error_name(entropic_error_t code) {
    switch (code) {
        case ENTROPIC_OK:                            return "ENTROPIC_OK";
        case ENTROPIC_ERROR_INVALID_ARGUMENT:        return "ENTROPIC_ERROR_INVALID_ARGUMENT";
        case ENTROPIC_ERROR_INVALID_CONFIG:          return "ENTROPIC_ERROR_INVALID_CONFIG";
        case ENTROPIC_ERROR_INVALID_STATE:           return "ENTROPIC_ERROR_INVALID_STATE";
        case ENTROPIC_ERROR_MODEL_NOT_FOUND:         return "ENTROPIC_ERROR_MODEL_NOT_FOUND";
        case ENTROPIC_ERROR_LOAD_FAILED:             return "ENTROPIC_ERROR_LOAD_FAILED";
        case ENTROPIC_ERROR_GENERATE_FAILED:         return "ENTROPIC_ERROR_GENERATE_FAILED";
        case ENTROPIC_ERROR_TOOL_NOT_FOUND:          return "ENTROPIC_ERROR_TOOL_NOT_FOUND";
        case ENTROPIC_ERROR_PERMISSION_DENIED:       return "ENTROPIC_ERROR_PERMISSION_DENIED";
        case ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH: return "ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH";
        case ENTROPIC_ERROR_PLUGIN_LOAD_FAILED:      return "ENTROPIC_ERROR_PLUGIN_LOAD_FAILED";
        case ENTROPIC_ERROR_TIMEOUT:                 return "ENTROPIC_ERROR_TIMEOUT";
        case ENTROPIC_ERROR_CANCELLED:               return "ENTROPIC_ERROR_CANCELLED";
        case ENTROPIC_ERROR_OUT_OF_MEMORY:           return "ENTROPIC_ERROR_OUT_OF_MEMORY";
        case ENTROPIC_ERROR_IO:                      return "ENTROPIC_ERROR_IO";
        case ENTROPIC_ERROR_INTERNAL:                return "ENTROPIC_ERROR_INTERNAL";
    }
    return "ENTROPIC_ERROR_UNKNOWN";
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
