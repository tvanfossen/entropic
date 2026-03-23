/**
 * @file entropic_hooks.cpp
 * @brief C API stub for hook registration.
 *
 * Implements entropic_register_hook() from entropic.h.
 * Returns ENTROPIC_ERROR_NOT_IMPLEMENTED until v1.9.1.
 *
 * @version 1.8.9
 */

#include <entropic/entropic.h>
#include <entropic/types/logging.h>

static auto logger = entropic::log::get("facade.hooks");

/**
 * @brief Register a hook callback (stub).
 * @param handle Engine handle.
 * @param hook_point Hook point enum value.
 * @param callback Callback function pointer.
 * @param user_data Opaque pointer.
 * @return ENTROPIC_ERROR_INVALID_HANDLE if handle is NULL,
 *         ENTROPIC_ERROR_INVALID_ARGUMENT if callback is NULL,
 *         ENTROPIC_ERROR_NOT_IMPLEMENTED (always, until v1.9.1).
 * @internal
 * @version 1.8.9
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_register_hook(
    entropic_handle_t handle,
    entropic_hook_point_t hook_point,
    entropic_hook_callback_t callback,
    void* user_data) {

    if (!handle) {
        return ENTROPIC_ERROR_INVALID_HANDLE;
    }
    if (!callback) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    (void)hook_point;
    (void)user_data;

    logger->info("entropic_register_hook: hook_point={} — NOT_IMPLEMENTED",
                 static_cast<int>(hook_point));
    return ENTROPIC_ERROR_NOT_IMPLEMENTED;
}
