// SPDX-License-Identifier: Apache-2.0
/**
 * @file tool_call_history.cpp
 * @brief ToolCallHistory ring buffer implementation.
 * @version 1.9.12
 */

#include <entropic/mcp/tool_call_history.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>

static auto logger = entropic::log::get("mcp.history");

namespace entropic {

/**
 * @brief Construct with buffer capacity.
 *
 * The buffer is allocated once, up front, and never grows — this is a
 * bounded diagnostic window (backing entropic.diagnose / entropic.inspect
 * and the validator's retry enrichment), explicitly NOT the audit log.
 *
 * @param capacity Maximum entries to retain (default 100).
 * @req REQ-MCP-020
 * @version 1.9.12
 */
ToolCallHistory::ToolCallHistory(size_t capacity)
    : capacity_(capacity) {
    buffer_.resize(capacity);
    logger->info("ToolCallHistory initialized (capacity={})", capacity);
}

/**
 * @brief Record a completed tool call.
 *
 * Takes the unique (writer) half of the shared_mutex. When the buffer is
 * full the oldest record is overwritten in place, so size saturates at
 * capacity rather than growing.
 *
 * @param entry Tool call record — sequence, fully-qualified tool name,
 *              key-only params summary, status, truncated result
 *              summary, elapsed ms, error detail and loop iteration.
 * @req REQ-MCP-020
 * @version 2.0.0
 */
void ToolCallHistory::record(const ToolCallRecord& entry) {
    std::unique_lock lock(mutex_);
    buffer_[head_] = entry;
    head_ = (head_ + 1) % capacity_;
    if (count_ < capacity_) {
        ++count_;
    }
    logger->info("Recorded: tool='{}', {}/{} slots",
                 entry.tool_name, count_, capacity_);
}

/**
 * @brief Get the N most recent entries (newest first).
 *
 * Takes the shared (reader) half of the mutex so several readers can run
 * concurrently alongside a single writer.
 *
 * @param count Maximum entries to return; clamped to the stored count.
 * @return Up to `count` records ordered newest-first, walking the ring
 *         backwards from the write head; empty when nothing is stored.
 * @req REQ-MCP-020
 * @version 1.9.12
 */
std::vector<ToolCallRecord> ToolCallHistory::recent(size_t count) const {
    std::shared_lock lock(mutex_);
    size_t n = std::min(count, count_);
    std::vector<ToolCallRecord> result;
    result.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        size_t idx = (head_ + capacity_ - 1 - i) % capacity_;
        result.push_back(buffer_[idx]);
    }
    return result;
}

/**
 * @brief Get all stored entries in insertion (oldest-first) order.
 *
 * Shared-lock read. Handles both the not-yet-wrapped case (start at 0)
 * and the wrapped case (start at the write head, which is the oldest
 * surviving slot).
 *
 * @return Every retained record oldest-first — at most capacity entries,
 *         the older ones having been overwritten.
 * @req REQ-MCP-020
 * @version 1.9.12
 */
std::vector<ToolCallRecord> ToolCallHistory::all() const {
    std::shared_lock lock(mutex_);
    std::vector<ToolCallRecord> result;
    result.reserve(count_);

    size_t start = (count_ < capacity_) ? 0
                                        : head_;
    for (size_t i = 0; i < count_; ++i) {
        size_t idx = (start + i) % capacity_;
        result.push_back(buffer_[idx]);
    }
    return result;
}

/**
 * @brief Serialize a single ToolCallRecord to JSON.
 * @param rec Record to serialize.
 * @return JSON object with sequence, tool_name, params_summary, status,
 *         result_summary, elapsed_ms and iteration always present, plus
 *         error_detail only when the record carries one.
 * @req REQ-MCP-020
 * @version 1.9.12
 */
static nlohmann::json record_to_json(const ToolCallRecord& rec) {
    nlohmann::json j;
    j["sequence"] = rec.sequence;
    j["tool_name"] = rec.tool_name;
    j["params_summary"] = rec.params_summary;
    j["status"] = rec.status;
    j["result_summary"] = rec.result_summary;
    j["elapsed_ms"] = rec.elapsed_ms;
    j["iteration"] = rec.iteration;
    if (!rec.error_detail.empty()) {
        j["error_detail"] = rec.error_detail;
    }
    return j;
}

/**
 * @brief Serialize recent entries to JSON array string.
 * @param count Maximum entries; 0 means "all", oldest-first.
 * @return Valid JSON array string honouring the count limit — oldest-first
 *         for count 0, newest-first otherwise; "[]" when nothing is
 *         stored.
 * @req REQ-MCP-020
 * @version 2.14.6
 */
std::string ToolCallHistory::to_json(size_t count) const {
    auto entries = (count == 0) ? all() : recent(count);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& rec : entries) {
        arr.push_back(record_to_json(rec));
    }
    // gh#216: never throw on bad bytes. The history is handle-wide and
    // re-serialized every run, so one record that cannot be dumped failed
    // every later turn on the host. Invalid UTF-8 becomes U+FFFD.
    return arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

/**
 * @brief Current number of stored entries.
 * @return Number of retained records under a shared lock — rises to
 *         capacity and then stays there as older entries are overwritten.
 * @req REQ-MCP-020
 * @version 1.9.12
 */
size_t ToolCallHistory::size() const {
    std::shared_lock lock(mutex_);
    return count_;
}

/**
 * @brief Extract top-level JSON keys as comma-separated summary.
 *
 * Keys ONLY — argument values never enter the history buffer.
 *
 * @param args_json Full JSON arguments string.
 * @return Comma-separated top-level key names; the input verbatim when
 *         it is not a JSON object or fails to parse.
 * @req REQ-MCP-020
 * @version 1.9.12
 */
std::string summarize_params(const std::string& args_json) {
    try {
        auto j = nlohmann::json::parse(args_json);
        if (!j.is_object()) {
            return args_json;
        }
        std::string result;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!result.empty()) {
                result += ", ";
            }
            result += it.key();
        }
        return result;
    } catch (...) {
        return args_json;
    }
}

/**
 * @brief Truncate text with "..." suffix if too long.
 *
 * gh#216 (v2.14.6): the cut backs up to a UTF-8 character boundary. A byte
 * cut split a multi-byte character (clew's U+2014), the result was invalid
 * UTF-8, and the handle-wide history then failed to serialize on every
 * later run. Same rule as the facade's utf8_safe_substr.
 *
 * @param text Input text.
 * @param max_len Maximum length in BYTES before truncation (200 for the
 *                history result summary).
 * @return The input unchanged when it is within `max_len`; otherwise its
 *         longest prefix of at most `max_len` bytes that ends on a character
 *         boundary, with "..." appended.
 * @req REQ-MCP-020
 * @version 2.14.6
 */
std::string truncate_result(const std::string& text, size_t max_len) {
    if (text.size() <= max_len) {
        return text;
    }
    size_t cut = max_len;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut) + "...";
}

} // namespace entropic
