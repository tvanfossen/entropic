/**
 * @file entropic_identity.cpp
 * @brief C API implementation for identity management.
 *
 * Implements entropic_load_identity() and entropic_get_identity()
 * from entropic.h. Currently stubs — real wiring to config/core
 * subsystems happens when the facade owns the full engine state.
 *
 * @version 1.8.9
 */

#include <entropic/entropic.h>
#include <entropic/types/logging.h>

static auto logger = entropic::log::get("facade.identity");

/**
 * @brief Load an identity by name from the configuration.
 * @param handle Engine handle.
 * @param identity_name Identity name string.
 * @return ENTROPIC_ERROR_INVALID_HANDLE if handle is NULL,
 *         ENTROPIC_ERROR_INVALID_ARGUMENT if identity_name is NULL,
 *         ENTROPIC_ERROR_IDENTITY_NOT_FOUND (stub — no config wiring yet).
 * @internal
 * @version 1.8.9
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_load_identity(
    entropic_handle_t handle,
    const char* identity_name) {

    if (!handle) {
        return ENTROPIC_ERROR_INVALID_HANDLE;
    }
    if (!identity_name) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    logger->info("entropic_load_identity: name='{}'", identity_name);
    return ENTROPIC_ERROR_IDENTITY_NOT_FOUND;
}

/**
 * @brief Get the current active identity as JSON.
 * @param handle Engine handle.
 * @param identity_json Output: JSON string.
 * @return ENTROPIC_ERROR_INVALID_HANDLE if handle is NULL,
 *         ENTROPIC_ERROR_INVALID_ARGUMENT if identity_json is NULL,
 *         ENTROPIC_ERROR_IDENTITY_NOT_FOUND (stub — no identity loaded).
 * @internal
 * @version 1.8.9
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_get_identity(
    entropic_handle_t handle,
    char** identity_json) {

    if (!handle) {
        return ENTROPIC_ERROR_INVALID_HANDLE;
    }
    if (!identity_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    logger->info("entropic_get_identity called");
    return ENTROPIC_ERROR_IDENTITY_NOT_FOUND;
}
