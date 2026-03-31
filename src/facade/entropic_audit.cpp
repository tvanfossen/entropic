/**
 * @file entropic_audit.cpp
 * @brief C API facade for audit log operations.
 *
 * Stubs for v1.9.5. Real implementations require engine wiring
 * (Phase 8, deferred). Validates inputs and returns appropriate
 * errors until the facade has a live AuditLogger.
 *
 * @version 1.9.5
 */

#include <entropic/entropic.h>
#include <entropic/types/logging.h>

static auto logger = entropic::log::get("facade.audit");

/**
 * @brief Flush the audit logger to disk.
 * @param handle Engine handle.
 * @return ENTROPIC_ERROR_INVALID_HANDLE if handle is NULL,
 *         ENTROPIC_ERROR_INVALID_STATE (stub — no engine wiring yet).
 * @internal
 * @version 1.9.5
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_audit_flush(entropic_handle_t handle) {
    if (!handle) {
        return ENTROPIC_ERROR_INVALID_HANDLE;
    }
    logger->info("entropic_audit_flush called (stub)");
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Get the number of audit log entries this session.
 * @param handle Engine handle.
 * @param count Output pointer.
 * @return ENTROPIC_ERROR_INVALID_HANDLE if handle is NULL,
 *         ENTROPIC_ERROR_INVALID_ARGUMENT if count is NULL,
 *         ENTROPIC_ERROR_INVALID_STATE (stub — no engine wiring yet).
 * @internal
 * @version 1.9.5
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_audit_count(entropic_handle_t handle, size_t* count) {
    if (!handle) {
        return ENTROPIC_ERROR_INVALID_HANDLE;
    }
    if (!count) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    logger->info("entropic_audit_count called (stub)");
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Read audit log entries from a JSONL file.
 * @param handle Engine handle.
 * @param path Path to audit.jsonl file.
 * @param filter_json Filter criteria JSON or NULL.
 * @param result_json Output: JSON array.
 * @return ENTROPIC_ERROR_INVALID_HANDLE if handle is NULL,
 *         ENTROPIC_ERROR_INVALID_ARGUMENT if path/result_json is NULL,
 *         ENTROPIC_ERROR_INVALID_STATE (stub — no engine wiring yet).
 * @internal
 * @version 1.9.5
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_audit_read(
    entropic_handle_t handle,
    const char* path,
    const char* /*filter_json*/,
    char** result_json) {
    if (!handle) {
        return ENTROPIC_ERROR_INVALID_HANDLE;
    }
    if (!path || !result_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    logger->info("entropic_audit_read called (stub): {}", path);
    return ENTROPIC_ERROR_INVALID_STATE;
}
