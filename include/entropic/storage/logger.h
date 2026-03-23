/**
 * @file logger.h
 * @brief Session log file management with spdlog rotating sinks.
 *
 * Creates session.log and session_model.log with size-based rotation.
 * Does NOT replace v1.8.0's global spdlog setup — adds file sinks for
 * session-specific output alongside structured logging.
 *
 * @version 1.8.8
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>

namespace spdlog { class logger; }

namespace entropic {

/**
 * @brief Configuration for session log files.
 * @version 1.8.8
 */
struct SessionLogConfig {
    std::filesystem::path log_dir;             ///< Directory for log files
    size_t max_file_size = 10 * 1024 * 1024;   ///< Max size before rotation (10MB)
    size_t max_files = 3;                       ///< Max rotated files to keep
};

/**
 * @brief Manages session.log and session_model.log with rotation.
 *
 * Creates spdlog rotating file sinks for:
 * - session.log: human-readable timeline of engine operations
 * - session_model.log: raw model input/output (prompt + completion)
 *
 * Thread-safe. Sinks are created once and shared across the session.
 *
 * @par Lifecycle:
 * @code
 *   SessionLogger logger(config);
 *   logger.initialize();
 *   logger.session_log(spdlog::level::info, "Engine started");
 *   logger.model_log("prompt text", "completion text", 42, 1500.0);
 * @endcode
 *
 * @version 1.8.8
 */
class SessionLogger {
public:
    /**
     * @brief Construct with configuration.
     * @param config Log file configuration.
     * @version 1.8.8
     */
    explicit SessionLogger(const SessionLogConfig& config);

    /**
     * @brief Destructor — flushes and drops loggers.
     * @version 1.8.8
     */
    ~SessionLogger();

    SessionLogger(const SessionLogger&) = delete;
    SessionLogger& operator=(const SessionLogger&) = delete;

    /**
     * @brief Initialize log sinks and open files.
     * @return true on success.
     * @version 1.8.8
     */
    bool initialize();

    /**
     * @brief Log a session event (human-readable timeline).
     * @param level Log level (info, warn, error, etc.).
     * @param message Event description.
     * @version 1.8.8
     */
    void session_log(int level, std::string_view message);

    /**
     * @brief Log raw model I/O.
     * @param prompt Full prompt sent to the model.
     * @param completion Raw model output.
     * @param tokens_generated Token count.
     * @param duration_ms Generation time in milliseconds.
     * @version 1.8.8
     */
    void model_log(std::string_view prompt,
                   std::string_view completion,
                   size_t tokens_generated,
                   double duration_ms);

    /**
     * @brief Check if logger is initialized.
     * @return true if sinks are active.
     * @version 1.8.8
     */
    bool is_initialized() const;

private:
    SessionLogConfig config_;                          ///< Configuration
    std::shared_ptr<spdlog::logger> session_logger_;   ///< Session log sink
    std::shared_ptr<spdlog::logger> model_logger_;     ///< Model log sink
};

} // namespace entropic
