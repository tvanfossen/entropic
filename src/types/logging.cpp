// SPDX-License-Identifier: Apache-2.0
/**
 * @file logging.cpp
 * @brief spdlog initialization, logger factory, and formatting utilities.
 * @version 2.0.7
 */

#include <entropic/types/logging.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/sink.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace entropic::log {

static std::once_flag s_init_flag;
static std::shared_ptr<spdlog::sinks::stderr_color_sink_mt> s_sink;

// gh#59 (v2.3.1): per-handle log dispatcher. Pre-v2.3.1, setup_session()
// called add_file_sink() which mutated EVERY registered logger globally
// — so two handles with different log_dirs would each see the other's
// log lines fan out into their session.log. The dispatcher solves this:
// a single sink installed once on the default logger consults a
// thread_local current_handle_id (set via HandleLogScope at API entry)
// and dispatches to per-handle file sinks. Logs emitted with no
// handle_id context are silently dropped from the file (they still hit
// the shared stderr console attached separately).

/// @brief Thread-local current handle id (0 = no scope active).
static thread_local int t_current_handle_id = 0;

/// @brief Per-handle file sink dispatcher (installed on default logger).
class HandleAwareSink final : public spdlog::sinks::sink {
public:
    /** @brief Route message to the current thread's handle file sink.
     * @internal @version 2.3.1 */
    void log(const spdlog::details::log_msg& msg) override {
        std::shared_ptr<spdlog::sinks::sink> target;
        int id = t_current_handle_id;
        {
            std::lock_guard lk(mu_);
            auto it = sinks_.find(id);
            if (it != sinks_.end()) { target = it->second; }
        }
        if (target) { target->log(msg); }
    }

    /** @brief Flush every registered handle sink. @internal @version 2.3.1 */
    void flush() override {
        std::vector<std::shared_ptr<spdlog::sinks::sink>> all;
        {
            std::lock_guard lk(mu_);
            all.reserve(sinks_.size());
            for (auto& [_, s] : sinks_) { all.push_back(s); }
        }
        for (auto& s : all) { s->flush(); }
    }

    /** @brief Apply pattern to all registered sinks. @internal @version 2.3.1 */
    void set_pattern(const std::string& pattern) override {
        std::lock_guard lk(mu_);
        pattern_ = pattern;
        for (auto& [_, s] : sinks_) { s->set_pattern(pattern); }
    }

    /** @brief Spdlog's set_formatter shim. @internal @version 2.3.1 */
    void set_formatter(std::unique_ptr<spdlog::formatter> f) override {
        // Forward only the pattern shape; the per-handle file sinks
        // need their own formatter instances since unique_ptr can't be
        // shared. spdlog uses set_pattern under the hood for typical
        // use, so this branch is rarely taken.
        std::lock_guard lk(mu_);
        if (!sinks_.empty()) {
            // Apply the same pattern by cloning via set_pattern when
            // possible; if formatter is a custom one, only the first
            // sink gets it. Pragmatic — the codebase always uses
            // set_pattern via set_default_logger's pattern.
            auto it = sinks_.begin();
            it->second->set_formatter(std::move(f));
        }
    }

    /** @brief Register/replace the sink for a handle id. @internal @version 2.3.1 */
    void register_sink(int id, std::shared_ptr<spdlog::sinks::sink> s) {
        std::lock_guard lk(mu_);
        sinks_[id] = std::move(s);
        if (!pattern_.empty()) { sinks_[id]->set_pattern(pattern_); }
    }

    /** @brief Remove a handle id's sink registration. @internal @version 2.3.1 */
    void unregister_sink(int id) {
        std::lock_guard lk(mu_);
        sinks_.erase(id);
    }

    /** @brief Snapshot of registered ids (tests + diagnostics).
     * @internal @version 2.3.1 */
    std::vector<int> registered_ids() const {
        std::lock_guard lk(mu_);
        std::vector<int> ids;
        ids.reserve(sinks_.size());
        for (auto& [id, _] : sinks_) { ids.push_back(id); }
        return ids;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<int, std::shared_ptr<spdlog::sinks::sink>> sinks_;
    std::string pattern_;
};

static std::shared_ptr<HandleAwareSink> s_dispatcher;

// gh#58 (v2.2.5): the v2.2.5 sink-dedup workaround. Kept around for
// backward compatibility with `setup_session()` callers that don't go
// through `register_handle_log()` yet (e.g. early-init / test fixtures
// without a handle id). The dispatcher path (v2.3.1) is preferred.
static std::mutex s_session_paths_mu;
static std::unordered_set<std::string> s_session_paths;

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
        // gh#59 (v2.3.1): install the per-handle dispatcher alongside
        // stderr on the default logger. Subsystems' loggers (created
        // via get()) inherit both sinks. The dispatcher routes file
        // writes based on thread_local current handle_id; stderr is
        // shared across handles (process-global console is acceptable
        // since operators reading stderr already see everything).
        s_dispatcher = std::make_shared<HandleAwareSink>();
        s_dispatcher->set_level(level);
        auto root = std::make_shared<spdlog::logger>(
            "entropic",
            spdlog::sinks_init_list{s_sink, s_dispatcher});
        spdlog::set_default_logger(root);
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
    // gh#67 (v2.3.2): historically this function attached a file sink
    // to the global spdlog logger tree and removed the stderr sink so
    // a single-handle process routed cleanly to session.log. After
    // v2.3.1 (gh#59) introduced per-handle routing via the
    // HandleAwareSink dispatcher, calling `register_handle_log()` from
    // configure_dir + configure_from_file already attaches the
    // handle's session.log file as a per-handle sink. If setup_session
    // ALSO does the legacy global add_file_sink path, every log line
    // ends up written twice — once via the dispatcher, once via the
    // duplicated global sink. That's the v2.3.1 doubled-line + SEGV
    // regression.
    //
    // The function stays in the public ABI because external callers
    // (TUI launchers, test harnesses) may call it directly. It now
    // does only the side-effect-free part: truncate session_model.log
    // so a fresh session starts clean. File-sink wiring is owned by
    // `register_handle_log()` exclusively.
    if (log_dir.empty()) { return; }
    auto model_file = log_dir / "session_model.log";
    std::error_code ec;
    std::filesystem::create_directories(model_file.parent_path(), ec);
    FILE* fp = fopen(model_file.string().c_str(), "w");
    if (fp) { fclose(fp); }
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

// ── gh#59 (v2.3.1): per-handle dispatcher API ─────────────────

/**
 * @brief gh#59 public entry — see header.
 * @utility
 * @version 2.3.1
 */
void register_handle_log(
    int handle_id, const std::filesystem::path& log_dir)
{
    if (handle_id == 0 || log_dir.empty()) { return; }
    if (!s_dispatcher) { init(spdlog::level::info); }

    auto log_file = log_dir / "session.log";
    std::filesystem::create_directories(log_file.parent_path());
    auto file_sink =
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            log_file.string(), /*truncate=*/true);
    file_sink->set_level(spdlog::level::trace);
    file_sink->set_pattern(
        "[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
    s_dispatcher->register_sink(handle_id, file_sink);

    // Match the v2.2.5 setup_session behavior: truncate
    // session_model.log too so a fresh session starts clean.
    auto model_file = log_dir / "session_model.log";
    FILE* fp = fopen(model_file.string().c_str(), "w");
    if (fp) { fclose(fp); }

    spdlog::flush_on(spdlog::level::trace);
}

/**
 * @brief gh#59 public entry — see header.
 * @utility
 * @version 2.3.1
 */
void unregister_handle_log(int handle_id) {
    if (!s_dispatcher || handle_id == 0) { return; }
    s_dispatcher->unregister_sink(handle_id);
}

/**
 * @brief gh#59 public entry — see header.
 * @utility
 * @version 2.3.1
 */
int current_handle_id() {
    return t_current_handle_id;
}

/**
 * @brief gh#59 RAII enter — see header.
 * @utility
 * @version 2.3.1
 */
HandleLogScope::HandleLogScope(int handle_id)
    : prev_id_(t_current_handle_id) {
    t_current_handle_id = handle_id;
}

/**
 * @brief gh#59 RAII exit — see header.
 * @utility
 * @version 2.3.1
 */
HandleLogScope::~HandleLogScope() {
    t_current_handle_id = prev_id_;
}

} // namespace entropic::log
