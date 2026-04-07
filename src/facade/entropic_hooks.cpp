/**
 * @file entropic_hooks.cpp
 * @brief C API implementation for hook registration and deregistration.
 *
 * v1.8.9: Returned ENTROPIC_ERROR_NOT_IMPLEMENTED.
 * v1.9.1: Full implementation delegating to HookRegistry.
 *
 * @version 2.0.0
 */

#include "engine_handle.h"

#include <entropic/entropic.h>
#include <entropic/types/logging.h>

static auto logger = entropic::log::get("facade.hooks");

/**
 * @brief Resolve HookRegistry from engine handle.
 *
 * Accesses the hook_registry member of the entropic_engine struct.
 * The handle is validated by the caller (null check before call).
 *
 * @param handle Engine handle (must not be NULL).
 * @return Pointer to the handle's HookRegistry.
 * @internal
 * @version 2.0.0
 */
static entropic::HookRegistry* get_registry(entropic_handle_t handle) {
    return &handle->hook_registry;
}

/**
 * @brief Register a hook callback for a hook point.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 1.9.1
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_register_hook(
    entropic_handle_t handle,
    entropic_hook_point_t hook_point,
    entropic_hook_callback_t callback,
    void* user_data,
    int priority) {

    if (!handle) {
        return ENTROPIC_ERROR_INVALID_HANDLE;
    }
    if (!callback) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    return get_registry(handle)->register_hook(
        hook_point, callback, user_data, priority);
}

/**
 * @brief Deregister a previously registered hook callback.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 1.9.1
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_deregister_hook(
    entropic_handle_t handle,
    entropic_hook_point_t hook_point,
    entropic_hook_callback_t callback,
    void* user_data) {

    if (!handle) {
        return ENTROPIC_ERROR_INVALID_HANDLE;
    }

    return get_registry(handle)->deregister_hook(
        hook_point, callback, user_data);
}
