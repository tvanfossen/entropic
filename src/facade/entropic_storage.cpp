/**
 * @file entropic_storage.cpp
 * @brief C API facade for storage open/close.
 *
 * Exposes storage lifecycle through the engine handle. The underlying
 * implementation is in librentropic-storage.so via i_storage_backend.h.
 *
 * @version 1.8.9
 */

#include <entropic/entropic.h>
#include <entropic/types/logging.h>

static auto logger = entropic::log::get("facade.storage");

/**
 * @brief Open or create a SQLite storage backend.
 * @param handle Engine handle.
 * @param db_path Path to SQLite database file.
 * @return ENTROPIC_ERROR_INVALID_HANDLE if handle is NULL,
 *         ENTROPIC_ERROR_INVALID_ARGUMENT if db_path is NULL,
 *         ENTROPIC_ERROR_INVALID_STATE (stub — no engine wiring yet).
 * @internal
 * @version 1.8.9
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_storage_open(
    entropic_handle_t handle,
    const char* db_path) {

    if (!handle) {
        return ENTROPIC_ERROR_INVALID_HANDLE;
    }
    if (!db_path) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    logger->info("entropic_storage_open: path='{}'", db_path);
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Close the storage backend.
 * @param handle Engine handle.
 * @return ENTROPIC_ERROR_INVALID_HANDLE if handle is NULL,
 *         ENTROPIC_OK (no-op if storage not opened).
 * @internal
 * @version 1.8.9
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_storage_close(entropic_handle_t handle) {
    if (!handle) {
        return ENTROPIC_ERROR_INVALID_HANDLE;
    }

    logger->info("entropic_storage_close called");
    return ENTROPIC_OK;
}
