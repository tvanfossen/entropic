/**
 * @file entropic_identity.cpp
 * @brief C API implementation for identity management.
 *
 * Implements entropic_load_identity() and entropic_get_identity()
 * from entropic.h. Currently stubs — real wiring to config/core
 * subsystems happens when the facade owns the full engine state.
 *
 * @version 2.0.0
 */

#include "engine_handle.h"

#include <entropic/entropic.h>
#include <entropic/types/logging.h>
#include <nlohmann/json.hpp>

static auto logger = entropic::log::get("facade.identity");

/**
 * @brief Check handle prerequisites for identity manager APIs.
 * @param h Engine handle.
 * @return ENTROPIC_OK if valid, error code otherwise.
 * @internal
 * @version 2.0.0
 */
static entropic_error_t check_identity_mgr(entropic_handle_t h) {
    if (!h) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    if (!h->identity_manager) { return ENTROPIC_ERROR_INVALID_STATE; }
    return ENTROPIC_OK;
}

/**
 * @brief Load an identity by name — verify it exists.
 *
 * @return ENTROPIC_OK if identity exists, error otherwise.
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_load_identity(
    entropic_handle_t handle,
    const char* identity_name) {

    auto rc = check_identity_mgr(handle);
    if (rc != ENTROPIC_OK || !identity_name) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    bool found = handle->identity_manager->has(identity_name);
    logger->info("load_identity: name='{}' found={}", identity_name, found);
    return found ? ENTROPIC_OK : ENTROPIC_ERROR_IDENTITY_NOT_FOUND;
}

/**
 * @brief Get an identity's config as JSON.
 *
 * @return ENTROPIC_OK on success, error otherwise.
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_get_identity(
    entropic_handle_t handle,
    char** identity_json) {

    auto rc = check_identity_mgr(handle);
    if (rc != ENTROPIC_OK || !identity_json) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    auto names = handle->identity_manager->list();
    auto* cfg = names.empty()
        ? nullptr : handle->identity_manager->get(names[0]);
    if (!cfg) { return ENTROPIC_ERROR_IDENTITY_NOT_FOUND; }

    nlohmann::json j;
    j["name"] = cfg->name;
    j["origin"] = (cfg->origin == entropic::IdentityOrigin::STATIC)
                   ? "static" : "dynamic";
    *identity_json = strdup(j.dump().c_str());
    return ENTROPIC_OK;
}
