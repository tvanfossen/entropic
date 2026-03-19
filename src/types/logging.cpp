/**
 * @file logging.cpp
 * @brief spdlog initialization and logger factory.
 * @version 1.8.0
 */

#include <entropic/types/logging.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <mutex>

namespace entropic::log {

static std::once_flag s_init_flag;
static std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> s_sink;

/**
 * @brief Initialize the logging subsystem.
 * @param level spdlog level. Subsequent calls are no-ops.
 * @utility
 * @version 1.8.0
 */
void init(spdlog::level::level_enum level) {
    std::call_once(s_init_flag, [level]() {
        s_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        s_sink->set_level(level);
        spdlog::set_default_logger(
            std::make_shared<spdlog::logger>("entropic", s_sink));
        spdlog::set_level(level);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    });
}

/**
 * @brief Get or create a named logger sharing the root sink.
 * @param name Logger name.
 * @return Shared pointer to the logger.
 * @version 1.8.0
 */
std::shared_ptr<spdlog::logger> get(const std::string& name) {
    auto logger = spdlog::get(name);
    if (logger) {
        return logger;
    }
    if (!s_sink) {
        init(spdlog::level::info);
    }
    logger = std::make_shared<spdlog::logger>(name, s_sink);
    spdlog::register_logger(logger);
    return logger;
}

} // namespace entropic::log
