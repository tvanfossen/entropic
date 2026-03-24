/**
 * @file entropic_hooks.cpp
 * @brief C API implementation for hook registration and deregistration.
 *
 * v1.8.9: Returned ENTROPIC_ERROR_NOT_IMPLEMENTED.
 * v1.9.1: Full implementation delegating to HookRegistry.
 *
 * @version 1.9.1
 */

#include <entropic/entropic.h>
#include <entropic/core/hook_registry.h>
#include <entropic/types/logging.h>

static auto logger = entropic::log::get("facade.hooks");

/**
 * @brief Resolve HookRegistry from engine handle.
 *
 * The engine handle layout is defined in the facade. This accessor
 * provides a stable way for hook APIs to reach the registry without
 * exposing the handle struct in public headers.
 *
 * @param handle Engine handle (must not be NULL).
 * @return Pointer to the handle's HookRegistry, or nullptr.
 * @internal
 * @version 1.9.1
 */
static entropic::HookRegistry* get_registry(entropic_handle_t handle) {
    // Until entropic_create() builds a full engine struct (v2.0),
    // the handle IS a HookRegistry pointer. The test/stub pattern
    // used by the facade creates handles via entropic_alloc().
    return reinterpret_cast<entropic::HookRegistry*>(handle);
}

/**
 * @brief Register a hook callback for a hook point.
 * @param handle Engine handle.
 * @param hook_point Hook point enum value.
 * @param callback Callback function pointer.
 * @param user_data Opaque pointer.
 * @param priority Execution order (0 = first).
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
 * @param handle Engine handle.
 * @param hook_point Hook point to deregister from.
 * @param callback Callback to remove.
 * @param user_data user_data from registration.
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
