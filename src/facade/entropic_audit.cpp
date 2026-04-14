/**
 * @file entropic_audit.cpp
 * @brief C API facade for audit log operations.
 *
 * Stubs for v1.9.5. Real implementations require engine wiring
 * (Phase 8, deferred). Validates inputs and returns appropriate
 * errors until the facade has a live AuditLogger.
 *
 * @version 2.0.0
 */

#include "engine_handle.h"

#include <entropic/entropic.h>
#include <entropic/types/logging.h>
#include "json_serializers.h"
#include <fstream>
#include <string>

static auto logger = entropic::log::get("facade.audit");

/**
 * @brief Flush the audit logger to disk.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_audit_flush(entropic_handle_t handle) {
    if (!handle) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    if (!handle->audit_logger) { return ENTROPIC_OK; }
    handle->audit_logger->flush();
    return ENTROPIC_OK;
}

/**
 * @brief Get the number of audit log entries this session.
 *
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_audit_count(entropic_handle_t handle, size_t* count) {
    if (!handle || !count) {
        return !handle ? ENTROPIC_ERROR_INVALID_HANDLE
                       : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    *count = handle->audit_logger
        ? handle->audit_logger->entry_count() : 0;
    return ENTROPIC_OK;
}

/**
 * @brief Read audit log entries from a JSONL file.
 *
 * @param path Filesystem path to the JSONL audit log file.
 * @param result_json Out-param: newly allocated JSON string (caller owns; free with entropic_free).
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_audit_read(
    entropic_handle_t handle,
    const char* path,
    const char* /*filter_json*/,
    char** result_json) {
    if (!handle || !path || !result_json) {
        return !handle ? ENTROPIC_ERROR_INVALID_HANDLE
                       : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::ifstream file(path);
        if (!file.is_open()) { throw std::runtime_error("cannot open"); }
        nlohmann::json arr = nlohmann::json::array();
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) { continue; }
            arr.push_back(nlohmann::json::parse(line));
        }
        *result_json = strdup(arr.dump().c_str());
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return ENTROPIC_ERROR_IO;
    }
}
