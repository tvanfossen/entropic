// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file entropic_storage.cpp
 * @brief C API facade for storage open/close.
 *
 * Exposes storage lifecycle through the engine handle. The underlying
 * implementation is in librentropic-storage.so via i_storage_backend.h.
 *
 * @version 2.0.0
 */

#include "engine_handle.h"

#include <entropic/entropic.h>
#include <entropic/types/logging.h>
#include <stdexcept>

static auto logger = entropic::log::get("facade.storage");

/**
 * @brief Open or create a SQLite storage backend.
 *
 * @param handle Engine handle returned by entropic_create.
 * @param db_path Filesystem path to the SQLite database file.
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_storage_open(
    entropic_handle_t handle,
    const char* db_path) {

    if (!handle || !db_path) {
        return !handle ? ENTROPIC_ERROR_INVALID_HANDLE
                       : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    try {
        handle->storage = std::make_unique<entropic::SqliteStorageBackend>(db_path);
        if (!handle->storage->initialize()) {
            handle->storage.reset();
            throw std::runtime_error("SQLite init failed");
        }
        logger->info("storage_open: path='{}'", db_path);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        logger->error("storage_open: {}", handle->last_error);
        return ENTROPIC_ERROR_STORAGE_FAILED;
    }
}

/**
 * @brief Close the storage backend.
 *
 * @param handle Engine handle returned by entropic_create.
 * @return ENTROPIC_OK (no-op if storage not opened).
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_storage_close(entropic_handle_t handle) {
    if (!handle) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    handle->storage.reset();
    logger->info("storage_close");
    return ENTROPIC_OK;
}
