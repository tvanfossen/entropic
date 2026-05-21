// SPDX-License-Identifier: Apache-2.0
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

/**
 * @brief Enable or disable the stderr console sink process-wide.
 *
 * When disabled, the shared stderr sink (s_sink) is stripped from the
 * default logger and every registered logger, and a process-global
 * flag makes `get()` filter it out of any logger created afterwards.
 * Engine output then routes to the per-handle file sink only.
 *
 * TUI consumers that paint to fd 2 (stderr) MUST disable this — an
 * engine log line on fd 2 corrupts the painted screen. Default state
 * (console enabled) is unchanged for operator/CLI consumers.
 *
 * @param enabled true to keep the console sink (default), false to
 *        remove it everywhere including future loggers.
 * @version 2.3.7
 */
ENTROPIC_EXPORT void set_console_enabled(bool enabled);

/**
 * @brief gh#59 (v2.3.1): register a per-handle session.log file.
 *
 * Replaces the old global-mutation behavior of `add_file_sink` for
 * the multi-handle case. Each handle registers its own log_dir under
 * a unique `handle_id`. The process-wide HandleAwareSink consults
 * the calling thread's current_handle_id (set via `HandleLogScope`)
 * and writes only to that handle's file. No cross-handle bleed.
 *
 * Idempotent: registering the same id twice replaces the old sink.
 * Safe to call before or after `init()`.
 *
 * @param handle_id Monotonic handle identifier (engine_handle::log_id).
 * @param log_dir Directory containing session.log + session_model.log.
 * @version 2.3.1
 */
ENTROPIC_EXPORT void register_handle_log(
    int handle_id,
    const std::filesystem::path& log_dir);

/**
 * @brief gh#59 (v2.3.1): release a handle's session.log file sink.
 *
 * Called from `entropic_destroy`. Closes the underlying file sink.
 * Safe to call on unregistered ids.
 *
 * @param handle_id Identifier previously passed to register_handle_log.
 * @version 2.3.1
 */
ENTROPIC_EXPORT void unregister_handle_log(int handle_id);

/**
 * @brief gh#59 (v2.3.1): RAII guard — sets thread's current handle_id.
 *
 * Construct at the top of every `entropic_*` API function so that any
 * log line emitted on this thread (directly or via subsystems called
 * from this thread) routes to this handle's session.log via the
 * HandleAwareSink. Nestable — saves/restores the previous id.
 *
 * Cross-thread: when spawning a background thread that should log on
 * a handle's behalf (external_bridge serve threads), wrap the thread
 * body in another HandleLogScope.
 *
 * @internal
 * @version 2.3.1
 */
class ENTROPIC_EXPORT HandleLogScope {
public:
    /**
     * @brief Enter scope; sets thread current handle_id.
     * @param handle_id The id to bind for this thread.
     * @return n/a (constructor).
     * @version 2.3.1
     */
    explicit HandleLogScope(int handle_id);
    /**
     * @brief Exit scope; restores previous handle_id.
     * @return n/a (destructor).
     * @version 2.3.1
     */
    ~HandleLogScope();
    HandleLogScope(const HandleLogScope&) = delete;
    HandleLogScope& operator=(const HandleLogScope&) = delete;
private:
    int prev_id_;
};

/**
 * @brief gh#59 (v2.3.1): query the current thread's handle_id.
 *
 * Returns 0 if no `HandleLogScope` is active on this thread. Mostly
 * for tests; production code should rely on the dispatcher routing.
 *
 * @version 2.3.1
 */
ENTROPIC_EXPORT int current_handle_id();

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
