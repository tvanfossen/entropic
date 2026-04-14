/**
 * @file audit_logger.h
 * @brief JSONL audit logger for MCP tool calls.
 *
 * Writes one JSON line per tool call to audit.jsonl in the session
 * directory. Append-only operation with thread-safe writes.
 *
 * Integrates with the hook system (v1.9.1) via POST_TOOL_CALL.
 * The AuditLogger is a passive observer: it does not modify or
 * cancel tool calls.
 *
 * @version 1.9.5
 */

#pragma once

#include <entropic/storage/audit_entry.h>
#include <entropic/types/config.h>
#include <entropic/types/hooks.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace entropic {

/**
 * @brief JSONL audit logger for MCP tool calls.
 *
 * Writes one JSON line per tool execution to audit.jsonl.
 * Thread-safe via write_mutex_. Atomic sequence counter ensures
 * total ordering even under concurrent writes.
 *
 * @par Lifecycle:
 * @code
 *   AuditLogger logger(config);
 *   logger.initialize();
 *   logger.record(entry);
 *   logger.flush();
 *   // Destructor flushes and closes
 * @endcode
 *
 * @par Thread safety:
 * record() and flush() acquire write_mutex_. Multiple threads
 * can call record() concurrently without corruption.
 *
 * @version 1.9.5
 */
class AuditLogger {
public:
    /**
     * @brief Construct with configuration.
     * @param config Audit log configuration.
     * @version 1.9.5
     */
    explicit AuditLogger(const AuditLogConfig& config);

    /**
     * @brief Destructor — flushes and closes the log file.
     * @version 1.9.5
     */
    ~AuditLogger();

    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    /**
     * @brief Open the log file and prepare for writing.
     *
     * Opens audit.jsonl in the configured log_dir. If the file
     * exists, new entries are APPENDED (not overwritten). The
     * sequence counter starts at 0 for each new logger instance
     * (it is monotonic per-session, not per-file).
     *
     * @return true on success, false on I/O error.
     * @version 1.9.5
     */
    bool initialize();

    /**
     * @brief Record a tool call audit entry.
     *
     * Serializes the entry to JSON, adds session-level metadata
     * (version, timestamp, session_id, sequence), and appends
     * one line to the file. Thread-safe.
     *
     * @param entry The audit log entry to write.
     * @version 1.9.5
     */
    void record(const AuditEntry& entry);

    /**
     * @brief Force flush buffered entries to disk.
     * @version 1.9.5
     */
    void flush();

    /**
     * @brief Get the number of entries recorded this session.
     * @return Entry count.
     * @version 1.9.5
     */
    size_t entry_count() const;

    /**
     * @brief Get the file path of the current audit log.
     * @return Absolute path to audit.jsonl.
     * @version 1.9.5
     */
    std::filesystem::path log_path() const;

    /**
     * @brief Hook callback for POST_TOOL_CALL integration.
     *
     * Static callback registered with HookRegistry. Extracts fields
     * from the hook context JSON and AuditHookContext (user_data),
     * constructs an AuditEntry, and calls record().
     *
     * Never modifies or cancels — sets *modified_json = NULL and
     * returns 0.
     *
     * @param hook_point Hook point (expected: ENTROPIC_HOOK_POST_TOOL_CALL).
     * @param context_json JSON with tool_name, args, result, elapsed_ms.
     * @param[out] modified_json Always set to NULL (no transformation).
     * @param user_data Pointer to AuditHookContext.
     * @return Always 0 (never cancels).
     * @callback
     * @version 1.9.5
     */
    static int hook_callback(
        entropic_hook_point_t hook_point,
        const char* context_json,
        char** modified_json,
        void* user_data);

private:
    AuditLogConfig config_;              ///< Configuration
    std::atomic<size_t> sequence_{0};    ///< Monotonic sequence counter
    std::atomic<size_t> entry_count_{0}; ///< Entries recorded this session
    std::mutex write_mutex_;             ///< Guards file writes
    std::ofstream file_;                 ///< Output file stream
    size_t buffered_count_ = 0;          ///< Entries since last flush
    size_t current_file_size_ = 0;       ///< Tracked file size for rotation

    /**
     * @brief Write a serialized JSON line under mutex.
     * @param line JSON string (no trailing newline).
     * @internal
     * @version 1.9.5
     */
    void write_line(const std::string& line);

    /**
     * @brief Generate ISO 8601 UTC timestamp with milliseconds.
     * @return Timestamp string (e.g., "2026-03-18T14:32:01.847Z").
     * @internal
     * @version 1.9.5
     */
    static std::string utc_timestamp();

    /**
     * @brief Rotate the log file if max_file_size is exceeded.
     *
     * Closes current file, renames audit.jsonl to audit.jsonl.1,
     * shifts existing rotated files, and opens a new audit.jsonl.
     *
     * @internal
     * @version 1.9.5
     */
    void rotate_if_needed();

    /**
     * @brief Perform the actual file rotation.
     * @internal
     * @version 1.9.5
     */
    void rotate_files();
};

} // namespace entropic
