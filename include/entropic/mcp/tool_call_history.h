/**
 * @file tool_call_history.h
 * @brief In-memory ring buffer of recent tool calls for introspection.
 *
 * Stores ToolCallRecord entries in a fixed-capacity ring buffer.
 * When full, new records overwrite the oldest. Thread-safe via
 * shared_mutex (multiple concurrent readers, single writer).
 *
 * This is NOT the v1.9.5 audit log. It is a lightweight diagnostic
 * window for entropic.diagnose and entropic.inspect tools.
 *
 * @version 1.9.12
 */

#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Lightweight record of a recent tool call for introspection.
 *
 * Stored in ToolCallHistory ring buffer. params_summary contains
 * only top-level JSON keys (no large values). result_summary is
 * truncated to 200 characters.
 *
 * @version 1.9.12
 */
struct ToolCallRecord {
    size_t sequence;              ///< Monotonic sequence number
    std::string tool_name;        ///< Fully-qualified tool name
    std::string params_summary;   ///< Top-level param keys only
    std::string status;           ///< "success", "error", "timeout"
    std::string result_summary;   ///< First 200 chars of result
    double elapsed_ms;            ///< Execution time in milliseconds
    std::string error_detail;     ///< Error message if status != "success"
    int iteration;                ///< Loop iteration when called
};

/**
 * @brief Fixed-size ring buffer of recent tool calls.
 *
 * Capacity set at construction (default 100). When full, oldest
 * entries are overwritten. Thread-safe reads via shared_mutex.
 *
 * @par Usage
 * @code
 *   ToolCallHistory history(100);
 *   history.record({.sequence=1, .tool_name="fs.read", ...});
 *   auto last5 = history.recent(5); // newest first
 *   auto all = history.all();       // oldest first
 * @endcode
 *
 * @version 1.9.12
 */
class ToolCallHistory {
public:
    /**
     * @brief Construct with buffer capacity.
     * @param capacity Maximum entries to retain (default 100).
     * @version 1.9.12
     */
    explicit ToolCallHistory(size_t capacity = 100);

    /**
     * @brief Record a completed tool call.
     * @param entry Tool call record to store.
     * @version 1.9.12
     */
    void record(const ToolCallRecord& entry);

    /**
     * @brief Get the N most recent entries (newest first).
     * @param count Maximum entries to return.
     * @return Vector of records, newest first.
     * @version 1.9.12
     */
    std::vector<ToolCallRecord> recent(size_t count) const;

    /**
     * @brief Get all stored entries (oldest first, insertion order).
     * @return Vector of all records.
     * @version 1.9.12
     */
    std::vector<ToolCallRecord> all() const;

    /**
     * @brief Serialize recent entries to JSON array string.
     * @param count Maximum entries to include (0 = all).
     * @return JSON array of ToolCallRecord objects.
     * @version 1.9.12
     */
    std::string to_json(size_t count) const;

    /**
     * @brief Current number of stored entries.
     * @return Entry count (<= capacity).
     * @version 1.9.12
     */
    size_t size() const;

private:
    std::vector<ToolCallRecord> buffer_; ///< Ring buffer storage
    size_t head_ = 0;                    ///< Next write position
    size_t count_ = 0;                   ///< Current entry count
    size_t capacity_;                    ///< Maximum capacity
    mutable std::shared_mutex mutex_;    ///< Reader/writer lock
};

/**
 * @brief Extract top-level JSON keys as a comma-separated summary.
 * @param args_json Full JSON arguments string.
 * @return Comma-separated key names (e.g., "path, content").
 * @utility
 * @version 1.9.12
 */
std::string summarize_params(const std::string& args_json);

/**
 * @brief Truncate a string to max_len characters with "..." suffix.
 * @param text Input text.
 * @param max_len Maximum length before truncation.
 * @return Truncated string, or original if short enough.
 * @utility
 * @version 1.9.12
 */
std::string truncate_result(const std::string& text, size_t max_len);

} // namespace entropic
