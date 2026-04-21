// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file logging.cpp
 * @brief spdlog initialization, logger factory, and formatting utilities.
 * @version 2.0.7
 */

#include <entropic/types/logging.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>

namespace entropic::log {

static std::once_flag s_init_flag;
static std::shared_ptr<spdlog::sinks::stderr_color_sink_mt> s_sink;

/**
 * @brief Initialize the logging subsystem.
 *
 * Uses stderr so that engine diagnostic output does not interleave
 * with the stdout streaming token callback used by TUI consumers.
 *
 * @param level spdlog level. Subsequent calls are no-ops.
 * @utility
 * @version 2.0.7
 */
void init(spdlog::level::level_enum level) {
    std::call_once(s_init_flag, [level]() {
        s_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        s_sink->set_level(level);
        spdlog::set_default_logger(
            std::make_shared<spdlog::logger>("entropic", s_sink));
        spdlog::set_level(level);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    });
}

/**
 * @brief Add a rotating file sink to all loggers.
 *
 * Creates the log directory, adds the sink to the default logger,
 * then walks every registered logger and adds the same sink.
 * Future loggers created via get() inherit it automatically.
 *
 * @param path Log file path.
 * @param max_size Max bytes before rotation.
 * @param max_files Rotated files to keep.
 * @utility
 * @version 2.0.1
 */
void add_file_sink(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        path.string(), true);  // truncate = true → fresh file each run
    file_sink->set_level(spdlog::level::trace);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");

    // Add to default logger (inherited by future get() calls)
    spdlog::default_logger()->sinks().push_back(file_sink);

    // Add to every already-registered logger
    spdlog::apply_all([&file_sink](std::shared_ptr<spdlog::logger> l) {
        l->sinks().push_back(file_sink);
    });

    // Flush on every message — session logs must survive crashes/early exits
    spdlog::flush_on(spdlog::level::trace);
}

/**
 * @brief Get or create a named logger sharing current sinks.
 *
 * New loggers inherit sinks from the default logger rather than
 * the original s_sink. This ensures loggers created after test
 * infrastructure replaces sinks via spdlog::apply_all() still
 * write to the correct destinations (e.g., per-test file sinks).
 *
 * @param name Logger name.
 * @return Shared pointer to the logger.
 * @version 1.10.4
 * @internal
 */
std::shared_ptr<spdlog::logger> get(const std::string& name) {
    auto logger = spdlog::get(name);
    if (logger) {
        return logger;
    }
    if (!s_sink) {
        init(spdlog::level::info);
    }
    auto default_logger = spdlog::default_logger();
    auto new_logger = std::make_shared<spdlog::logger>(
        name, default_logger->sinks().begin(),
        default_logger->sinks().end());
    new_logger->set_level(default_logger->level());
    spdlog::register_logger(new_logger);
    return new_logger;
}

/**
 * @brief Set up session logging for a project directory.
 *
 * Adds a spdlog file sink for session.log (captures all engine
 * operational logging). Truncates session_model.log for a fresh
 * session (raw streaming user/assistant content).
 *
 * @param log_dir Directory for session log files.
 * @utility
 * @version 2.0.1
 */
/**
 * @brief Remove the console (stderr) sink from all loggers.
 *
 * Called after adding a file sink to ensure logs go to exactly
 * one destination. Matches by pointer identity against s_sink.
 *
 * @utility
 * @version 2.0.7
 */
static void remove_console_sink() {
    if (!s_sink) { return; }
    spdlog::apply_all([](std::shared_ptr<spdlog::logger> l) {
        auto& sinks = l->sinks();
        sinks.erase(
            std::remove(sinks.begin(), sinks.end(), s_sink),
            sinks.end());
    });
}

/**
 * @brief Set up session logging for a project directory.
 *
 * Adds a file sink for session.log, removes the console sink so
 * logs go to exactly one destination. Truncates session_model.log.
 *
 * @param log_dir Directory for session log files.
 * @utility
 * @version 2.0.6
 */
void setup_session(const std::filesystem::path& log_dir) {
    if (log_dir.empty()) { return; }

    auto log_file = log_dir / "session.log";
    add_file_sink(log_file);
    remove_console_sink();

    auto model_file = log_dir / "session_model.log";
    std::filesystem::create_directories(model_file.parent_path());
    FILE* fp = fopen(model_file.string().c_str(), "w");
    if (fp) { fclose(fp); }

    auto logger = get("log");
    logger->info("session log: {}", log_file.string());
    logger->info("model log: {}", model_file.string());
}

// ── Content escaping ──────────────────────────────────────────

/**
 * @brief Consume an ANSI CSI sequence starting at the ESC byte.
 *
 * Writes "[ESC<params>]" into result. Advances i past the
 * sequence terminator 'm'. If the sequence is malformed, writes
 * "[ESC]" and leaves i on the ESC byte.
 *
 * @param text Source string.
 * @param i Current index (on the ESC byte). Updated in place.
 * @param result Destination string.
 * @utility
 * @version 1.10.4
 */
void escape_ansi(const std::string& text, size_t& i,
                 std::string& result)
{
    result += "[ESC";
    if (i + 1 >= text.size() || text[i + 1] != '[') {
        result += ']';
        return;
    }
    ++i; // skip '['
    while (i + 1 < text.size() && text[i + 1] != 'm') {
        ++i;
        result += text[i];
    }
    if (i + 1 < text.size()) { ++i; } // skip 'm'
    result += ']';
}

/**
 * @brief 256-entry table mapping bytes to escape strings (nullptr = passthrough).
 * @return Reference to static lookup table.
 * @internal
 * @version 1.10.4
 */
static const std::array<const char*, 256>& escape_table() {
    static const auto tbl = []() {
        std::array<const char*, 256> t{};
        t[static_cast<unsigned char>('\n')] = "\\n";
        t[static_cast<unsigned char>('\r')] = "\\r";
        t[static_cast<unsigned char>('\t')] = "\\t";
        t[static_cast<unsigned char>('{')] = "{{";
        t[static_cast<unsigned char>('}')] = "}}";
        return t;
    }();
    return tbl;
}

/**
 * @brief Append escaped form of c if special, else return false.
 * @param c Character to test.
 * @param result Destination string.
 * @return True if c was escaped, false for passthrough.
 * @utility
 * @version 1.10.4
 */
bool escape_char(char c, std::string& result) {
    const char* esc = escape_table()[static_cast<unsigned char>(c)];
    if (!esc) { return false; }
    result += esc;
    return true;
}

/**
 * @brief Escape content for safe spdlog formatting.
 *
 * Replaces embedded newlines with \\n, carriage returns with \\r,
 * ANSI escape sequences with [ESC...], and curly braces with doubled
 * braces so spdlog/fmt does not interpret them as format specifiers.
 *
 * @param text Raw content.
 * @return Escaped string safe for spdlog.
 * @utility
 * @version 1.10.4
 */
std::string escape_content(const std::string& text) {
    std::string result;
    result.reserve(text.size() + text.size() / 8);
    for (size_t i = 0; i < text.size(); ++i) {
        if (escape_char(text[i], result)) { continue; }
        if (text[i] == '\033') { escape_ansi(text, i, result); }
        else { result += text[i]; }
    }
    return result;
}

// ── Formatting helpers ────────────────────────────────────────

/**
 * @brief Log full content with escaping applied.
 * @param logger Target logger.
 * @param level spdlog level.
 * @param label Prefix label.
 * @param text Raw text — escaped before formatting.
 * @utility
 * @version 1.10.4
 */
void log_content(
    const std::shared_ptr<spdlog::logger>& logger,
    spdlog::level::level_enum level,
    const std::string& label,
    const std::string& text)
{
    logger->log(level, "{}: {}", label, escape_content(text));
}

/**
 * @brief Log a timing measurement.
 * @param logger Target logger.
 * @param label Operation label.
 * @param ms Elapsed milliseconds.
 * @utility
 * @version 1.10.4
 */
void log_timing(
    const std::shared_ptr<spdlog::logger>& logger,
    const std::string& label,
    double ms)
{
    logger->info("{}: {:.1f}ms", label, ms);
}

/**
 * @brief Log token count with throughput.
 * @param logger Target logger.
 * @param count Token count.
 * @param time_ms Generation time in milliseconds.
 * @utility
 * @version 1.10.4
 */
void log_tokens(
    const std::shared_ptr<spdlog::logger>& logger,
    int count,
    double time_ms)
{
    double tok_s = (time_ms > 0.0)
        ? (static_cast<double>(count) / time_ms * 1000.0) : 0.0;
    logger->info("{} tokens, {:.1f}ms, {:.1f} tok/s",
                 count, time_ms, tok_s);
}

/**
 * @brief Log a decision/routing event.
 * @param logger Target logger.
 * @param label Decision category.
 * @param key Decision input.
 * @param value Decision output.
 * @utility
 * @version 1.10.4
 */
void log_decision(
    const std::shared_ptr<spdlog::logger>& logger,
    const std::string& label,
    const std::string& key,
    const std::string& value)
{
    logger->info("{}: {} -> {}", label, key, value);
}

} // namespace entropic::log
