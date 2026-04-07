/**
 * @file audit_logger.cpp
 * @brief AuditLogger implementation — JSONL recording and hook callback.
 * @version 1.9.5
 */

#include <entropic/storage/audit_logger.h>
#include <entropic/storage/audit_hook_context.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <iomanip>
#include <sstream>

static auto logger = entropic::log::get("storage.audit_logger");

namespace entropic {

/**
 * @brief Construct with configuration.
 * @param config Audit log configuration.
 * @version 1.9.5
 */
AuditLogger::AuditLogger(const AuditLogConfig& config)
    : config_(config) {}

/**
 * @brief Destructor — flushes and closes the log file.
 * @version 1.9.5
 */
AuditLogger::~AuditLogger() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

/**
 * @brief Open the log file and prepare for writing.
 * @return true on success, false on I/O error.
 * @internal
 * @version 1.9.5
 */
bool AuditLogger::initialize() {
    if (!config_.enabled) {
        logger->info("Audit logging disabled");
        return true;
    }
    std::filesystem::create_directories(config_.log_dir);
    auto path = config_.log_dir / "audit.jsonl";
    current_file_size_ = std::filesystem::exists(path)
        ? std::filesystem::file_size(path) : 0;
    file_.open(path, std::ios::app);
    if (!file_.is_open()) {
        logger->error("Failed to open audit log: {}", path.string());
        return false;
    }
    logger->info("Audit log opened: {}", path.string());
    return true;
}

/**
 * @brief Record a tool call audit entry.
 * @param entry The audit log entry to write.
 * @internal
 * @version 2.0.0
 */
void AuditLogger::record(const AuditEntry& entry) {
    if (!config_.enabled || !file_.is_open()) {
        return;
    }
    nlohmann::json line = audit_entry_to_json(entry);
    line["version"] = 1;
    line["timestamp"] = utc_timestamp();
    line["session_id"] = config_.session_id;
    line["sequence"] = static_cast<int64_t>(sequence_.fetch_add(1));

    std::string serialized = line.dump();
    write_line(serialized);
    auto seq = entry_count_.fetch_add(1);
    logger->info("Audit: tool='{}', caller='{}', seq={}",
                 entry.tool_name, entry.caller_id, seq);
}

/**
 * @brief Write a serialized JSON line under mutex.
 * @param line JSON string (no trailing newline).
 * @internal
 * @version 1.9.5
 */
void AuditLogger::write_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    rotate_if_needed();
    file_ << line << '\n';
    current_file_size_ += line.size() + 1;
    buffered_count_++;
    bool should_flush = (config_.flush_interval_entries == 0)
        || (buffered_count_ >= config_.flush_interval_entries);
    if (should_flush) {
        file_.flush();
        buffered_count_ = 0;
    }
}

/**
 * @brief Force flush buffered entries to disk.
 * @internal
 * @version 1.9.5
 */
void AuditLogger::flush() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (file_.is_open()) {
        file_.flush();
        buffered_count_ = 0;
    }
}

/**
 * @brief Get the number of entries recorded this session.
 * @return Entry count.
 * @internal
 * @version 1.9.5
 */
size_t AuditLogger::entry_count() const {
    return entry_count_.load();
}

/**
 * @brief Get the file path of the current audit log.
 * @return Absolute path to audit.jsonl.
 * @internal
 * @version 1.9.5
 */
std::filesystem::path AuditLogger::log_path() const {
    return config_.log_dir / "audit.jsonl";
}

/**
 * @brief Generate ISO 8601 UTC timestamp with milliseconds.
 * @return Timestamp string.
 * @internal
 * @version 1.9.5
 */
std::string AuditLogger::utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream ss;
    ss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return ss.str();
}

/**
 * @brief Rotate the log file if max_file_size is exceeded.
 * @internal
 * @version 1.9.5
 */
void AuditLogger::rotate_if_needed() {
    if (config_.max_file_size == 0) {
        return;
    }
    if (current_file_size_ < config_.max_file_size) {
        return;
    }
    rotate_files();
}

/**
 * @brief Perform the actual file rotation.
 * @internal
 * @version 1.9.5
 */
void AuditLogger::rotate_files() {
    file_.close();
    auto base = config_.log_dir / "audit.jsonl";
    // Shift existing rotated files (N → N+1), drop oldest
    for (size_t i = config_.max_files; i >= 1; --i) {
        auto src = base.string() + "." + std::to_string(i);
        auto dst = base.string() + "." + std::to_string(i + 1);
        if (i == config_.max_files) {
            std::filesystem::remove(src);
        } else if (std::filesystem::exists(src)) {
            std::filesystem::rename(src, dst);
        }
    }
    std::filesystem::rename(base, base.string() + ".1");
    file_.open(base, std::ios::app);
    current_file_size_ = 0;
    logger->info("Audit log rotated");
}

/**
 * @brief Hook callback for POST_TOOL_CALL integration.
 * @param hook_point Hook point.
 * @param context_json JSON with tool execution details.
 * @param[out] modified_json Always NULL.
 * @param user_data Pointer to AuditHookContext.
 * @return Always 0.
 * @callback
 * @version 1.9.5
 */
int AuditLogger::hook_callback(
    entropic_hook_point_t /*hook_point*/,
    const char* context_json,
    char** modified_json,
    void* user_data) {
    *modified_json = nullptr;
    auto* ctx = static_cast<AuditHookContext*>(user_data);
    if (ctx == nullptr || ctx->logger == nullptr) {
        return 0;
    }
    try {
        auto j = nlohmann::json::parse(context_json);
        AuditEntry entry;
        entry.tool_name = j.value("tool_name", "");
        entry.params_json = j.contains("args")
            ? j["args"].dump() : "{}";
        entry.result_content = j.value("result", "");
        entry.elapsed_ms = j.value("elapsed_ms", 0.0);
        entry.directives_json = j.contains("directives")
            ? j["directives"].dump() : "[]";
        populate_from_hook_context(entry, *ctx);
        ctx->logger->record(entry);
    } catch (const std::exception& e) {
        logger->error("Audit hook callback failed: {}", e.what());
    }
    return 0;
}

/**
 * @brief Populate AuditEntry fields from AuditHookContext state.
 * @param entry Entry to populate.
 * @param ctx Hook context with engine state pointers.
 * @internal
 * @version 1.9.5
 */
void populate_from_hook_context(AuditEntry& entry,
                                const AuditHookContext& ctx) {
    entry.caller_id = ctx.caller_id ? *ctx.caller_id : "unknown";
    entry.delegation_depth = ctx.delegation_depth ? *ctx.delegation_depth : 0;
    entry.iteration = ctx.iteration ? *ctx.iteration : 0;
    entry.parent_conversation_id = ctx.parent_conversation_id
        ? *ctx.parent_conversation_id : "";
    entry.result_status = entry.result_content.empty()
        ? "error" : "success";
}

} // namespace entropic
