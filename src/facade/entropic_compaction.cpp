/**
 * @file entropic_compaction.cpp
 * @brief C API facade for compaction operations.
 *
 * Implements entropic_compact, entropic_register_compactor,
 * entropic_deregister_compactor, and entropic_get_default_compactor.
 *
 * @version 2.0.0
 */

#include "engine_handle.h"

#include <entropic/entropic.h>
#include <entropic/types/logging.h>

static auto logger = entropic::log::get("facade.compaction");

/**
 * @brief Check handle prerequisites for compaction APIs.
 * @param h Engine handle.
 * @return ENTROPIC_OK if valid, error code otherwise.
 * @internal
 * @version 2.0.0
 */
static entropic_error_t check_compactor(entropic_handle_t h) {
    if (!h) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    if (!h->compactor_registry) { return ENTROPIC_ERROR_INVALID_STATE; }
    return ENTROPIC_OK;
}

/**
 * @brief Trigger compaction on current context.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_compact(
    entropic_handle_t handle,
    const char* identity,
    char** result_json) {

    auto rc = check_compactor(handle);
    if (rc != ENTROPIC_OK || !handle->engine) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_STATE;
    }

    // Compaction runs through the engine's CompactionManager during
    // the agentic loop. External compact requires an active session.
    (void)identity;
    (void)result_json;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Register a custom compactor for an identity.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_register_compactor(
    entropic_handle_t handle,
    const char* identity,
    entropic_compactor_fn compactor,
    void* user_data) {

    auto rc = check_compactor(handle);
    if (rc != ENTROPIC_OK || !compactor) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_CONFIG;
    }

    logger->info("register_compactor: identity='{}'",
                 identity ? identity : "(global)");
    handle->compactor_registry->register_compactor(
        identity ? identity : "", compactor, user_data);
    return ENTROPIC_OK;
}

/**
 * @brief Deregister a custom compactor for an identity.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_deregister_compactor(
    entropic_handle_t handle,
    const char* identity) {

    auto rc = check_compactor(handle);
    if (rc != ENTROPIC_OK) { return rc; }

    handle->compactor_registry->deregister_compactor(
        identity ? identity : "");
    return ENTROPIC_OK;
}

/**
 * @brief Get the built-in default compactor function pointer.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_get_default_compactor(
    entropic_handle_t handle,
    entropic_compactor_fn* compactor,
    void** user_data) {

    auto rc = check_compactor(handle);
    if (rc != ENTROPIC_OK || !compactor) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    // Default compactor is managed internally by CompactorRegistry.
    // Consumers wrap by registering a custom compactor that calls
    // entropic_compact() internally. No raw C function pointer
    // exposed for the built-in strategy.
    *compactor = nullptr;
    if (user_data) { *user_data = nullptr; }
    return ENTROPIC_OK;
}
