/**
 * @file logging.h
 * @brief spdlog initialization and logger access.
 *
 * Every .so in the entropic project uses spdlog for structured logging.
 * This header establishes the initialization pattern used from v1.8.0
 * onward. All loggers are created through this interface.
 *
 * @par Pattern
 * Each library creates a named logger via entropic::log::get("libname").
 * The facade initializes the root sink configuration (level, format, output).
 * Libraries call get() lazily — sink config propagates automatically via
 * spdlog's registry.
 *
 * @par Thread safety
 * spdlog loggers are thread-safe. Initialization (init_logging) must
 * happen once from a single thread before any logging calls.
 *
 * @version 1.10.4
 */

#pragma once

#include <entropic/entropic_export.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace entropic::log {

/**
 * @brief Initialize the logging subsystem.
 *
 * Call once at engine startup (in entropic_create). Sets the global
 * log level and output format. Subsequent calls are no-ops.
 *
 * @param level spdlog level (trace, debug, info, warn, error, critical).
 * @version 1.8.0
 */
ENTROPIC_EXPORT void init(spdlog::level::level_enum level = spdlog::level::info);

/**
 * @brief Add a file sink to all loggers (truncated per run).
 *
 * Creates the parent directory, opens the file (truncating any
 * existing content), adds the sink to the default logger, and
 * applies it to every logger already registered. Loggers created
 * after this call inherit the sink automatically via get().
 *
 * @param path Log file path.
 * @version 2.0.1
 */
ENTROPIC_EXPORT void add_file_sink(const std::filesystem::path& path);

/**
 * @brief Get or create a named logger.
 *
 * Returns the existing logger if already created, or creates a new one
 * sharing the root sink configuration.
 *
 * @param name Logger name (e.g., "types", "inference", "mcp").
 * @return Shared pointer to the logger.
 *
 * @par Example
 * @code
 * auto log = entropic::log::get("inference");
 * log->info("Model loaded: {} ({:.1f} GB)", path, size_gb);
 * log->error("Load failed: {}", entropic_error_name(err));
 * @endcode
 * @version 1.8.0
 */
ENTROPIC_EXPORT std::shared_ptr<spdlog::logger> get(const std::string& name);

/**
 * @brief Set up session logging for a project directory.
 *
 * Adds a spdlog file sink for session.log and truncates
 * session_model.log for a fresh session. Centralizes log
 * file lifecycle that was previously inline in the facade.
 *
 * @param log_dir Directory for session log files.
 * @version 2.0.1
 */
ENTROPIC_EXPORT void setup_session(const std::filesystem::path& log_dir);

// ── Timing utilities ──────────────────────────────────────────

/**
 * @brief Get current time for timing measurements.
 * @return Steady clock time point.
 * @utility
 * @version 1.10.4
 */
inline auto now() { return std::chrono::steady_clock::now(); }

/**
 * @brief Compute elapsed milliseconds between two time points.
 * @param start Start time point.
 * @param end End time point.
 * @return Elapsed time in milliseconds.
 * @utility
 * @version 1.10.4
 */
inline double elapsed_ms(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end)
{
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        end - start);
    return static_cast<double>(us.count()) / 1000.0;
}

// ── Content escaping ──────────────────────────────────────────

/**
 * @brief Escape content for safe spdlog formatting.
 *
 * Replaces embedded newlines with \\n, ANSI escape sequences with
 * their hex representation, and curly braces (fmt specifiers) with
 * doubled braces so spdlog/fmt does not interpret them.
 *
 * @param text Raw content (user input or model output).
 * @return Escaped string safe for spdlog format strings.
 * @utility
 * @version 1.10.4
 */
ENTROPIC_EXPORT std::string escape_content(const std::string& text);

// ── Formatting helpers ────────────────────────────────────────

/**
 * @brief Log full content with escaping applied.
 * @param logger Target logger.
 * @param level spdlog level.
 * @param label Prefix label (e.g., "Content", "Input").
 * @param text Raw text — escaped before formatting.
 * @utility
 * @version 1.10.4
 */
ENTROPIC_EXPORT void log_content(
    const std::shared_ptr<spdlog::logger>& logger,
    spdlog::level::level_enum level,
    const std::string& label,
    const std::string& text);

/**
 * @brief Log a timing measurement.
 * @param logger Target logger.
 * @param label Operation label.
 * @param ms Elapsed milliseconds.
 * @utility
 * @version 1.10.4
 */
ENTROPIC_EXPORT void log_timing(
    const std::shared_ptr<spdlog::logger>& logger,
    const std::string& label,
    double ms);

/**
 * @brief Log token count with throughput.
 * @param logger Target logger.
 * @param count Token count.
 * @param time_ms Generation time in milliseconds.
 * @utility
 * @version 1.10.4
 */
ENTROPIC_EXPORT void log_tokens(
    const std::shared_ptr<spdlog::logger>& logger,
    int count,
    double time_ms);

/**
 * @brief Log a decision/routing event.
 * @param logger Target logger.
 * @param label Decision category (e.g., "Route", "Grammar").
 * @param key Decision input.
 * @param value Decision output.
 * @utility
 * @version 1.10.4
 */
ENTROPIC_EXPORT void log_decision(
    const std::shared_ptr<spdlog::logger>& logger,
    const std::string& label,
    const std::string& key,
    const std::string& value);

} // namespace entropic::log
