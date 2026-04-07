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
 * @param capacity Maximum entries to retain.
 * @internal
 * @version 1.9.12
 */
ToolCallHistory::ToolCallHistory(size_t capacity)
    : capacity_(capacity) {
    buffer_.resize(capacity);
    logger->info("ToolCallHistory initialized (capacity={})", capacity);
}

/**
 * @brief Record a completed tool call.
 * @param entry Tool call record.
 * @internal
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
 * @param count Maximum entries to return.
 * @return Vector of records, newest first.
 * @internal
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
 * @return Vector of all records.
 * @internal
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
 * @return JSON object.
 * @internal
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
 * @param count Maximum entries (0 = all).
 * @return JSON array string.
 * @internal
 * @version 1.9.12
 */
std::string ToolCallHistory::to_json(size_t count) const {
    auto entries = (count == 0) ? all() : recent(count);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& rec : entries) {
        arr.push_back(record_to_json(rec));
    }
    return arr.dump();
}

/**
 * @brief Current number of stored entries.
 * @return Entry count.
 * @internal
 * @version 1.9.12
 */
size_t ToolCallHistory::size() const {
    std::shared_lock lock(mutex_);
    return count_;
}

/**
 * @brief Extract top-level JSON keys as comma-separated summary.
 * @param args_json Full JSON arguments string.
 * @return Comma-separated key names.
 * @internal
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
 * @param text Input text.
 * @param max_len Maximum length before truncation.
 * @return Truncated or original string.
 * @internal
 * @version 1.9.12
 */
std::string truncate_result(const std::string& text, size_t max_len) {
    if (text.size() <= max_len) {
        return text;
    }
    return text.substr(0, max_len) + "...";
}

} // namespace entropic
