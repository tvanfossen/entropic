/**
 * @file logger.cpp
 * @brief SessionLogger implementation with spdlog rotating file sinks.
 * @version 1.8.8
 */

#include <entropic/storage/logger.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <filesystem>

namespace entropic {

/**
 * @brief Construct with configuration.
 * @param config Log file configuration.
 * @internal
 * @version 1.8.8
 */
SessionLogger::SessionLogger(const SessionLogConfig& config)
    : config_(config) {}

/**
 * @brief Destructor — flushes and drops loggers.
 * @internal
 * @version 1.8.8
 */
SessionLogger::~SessionLogger() {
    if (session_logger_) {
        session_logger_->flush();
        spdlog::drop("entropic_session");
    }
    if (model_logger_) {
        model_logger_->flush();
        spdlog::drop("entropic_model");
    }
}

/**
 * @brief Initialize rotating file sinks.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SessionLogger::initialize() {
    std::filesystem::create_directories(config_.log_dir);

    auto session_path = config_.log_dir / "session.log";
    auto model_path = config_.log_dir / "session_model.log";

    try {
        session_logger_ = spdlog::rotating_logger_mt(
            "entropic_session",
            session_path.string(),
            config_.max_file_size,
            config_.max_files);
        session_logger_->set_pattern(
            "[%Y-%m-%d %H:%M:%S] [%l] %v");

        model_logger_ = spdlog::rotating_logger_mt(
            "entropic_model",
            model_path.string(),
            config_.max_file_size,
            config_.max_files);
        model_logger_->set_pattern("[%Y-%m-%d %H:%M:%S] %v");

        session_logger_->info("Session log started");
        model_logger_->info("Model output log started");
        return true;
    } catch (const spdlog::spdlog_ex& ex) {
        spdlog::error("SessionLogger init failed: {}", ex.what());
        return false;
    }
}

/**
 * @brief Log a session event.
 * @param level spdlog level (cast from int).
 * @param message Event description.
 * @internal
 * @version 1.8.8
 */
void SessionLogger::session_log(int level, std::string_view message) {
    if (!session_logger_) return;
    session_logger_->log(
        static_cast<spdlog::level::level_enum>(level),
        "{}", message);
}

/**
 * @brief Log raw model I/O.
 * @param prompt Full prompt.
 * @param completion Model output.
 * @param tokens_generated Token count.
 * @param duration_ms Generation time.
 * @internal
 * @version 1.8.8
 */
void SessionLogger::model_log(
        std::string_view prompt,
        std::string_view completion,
        size_t tokens_generated,
        double duration_ms) {
    if (!model_logger_) return;
    model_logger_->info(
        "--- PROMPT ---\n{}\n--- COMPLETION ({} tokens, {:.1f}ms) ---\n{}",
        prompt, tokens_generated, duration_ms, completion);
}

/**
 * @brief Check if logger is initialized.
 * @return true if sinks are active.
 * @internal
 * @version 1.8.8
 */
bool SessionLogger::is_initialized() const {
    return session_logger_ != nullptr && model_logger_ != nullptr;
}

} // namespace entropic
