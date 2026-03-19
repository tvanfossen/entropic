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
 * @version 1.8.0
 */

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
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
void init(spdlog::level::level_enum level = spdlog::level::info);

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
std::shared_ptr<spdlog::logger> get(const std::string& name);

} // namespace entropic::log
