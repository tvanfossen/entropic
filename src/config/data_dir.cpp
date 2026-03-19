/**
 * @file data_dir.cpp
 * @brief Bundled data directory resolution.
 * @version 1.8.1
 */

#include <entropic/config/loader.h>
#include <entropic/entropic_config.h>
#include <entropic/types/logging.h>

static auto s_log = entropic::log::get("config");

namespace entropic::config {

/**
 * @brief Resolve the bundled data directory.
 *
 * Priority:
 * 1. config.config_dir / "data" (if config_dir non-empty and exists)
 * 2. CONFIG_ENTROPIC_DATA_DIR (compile-time install path)
 * 3. Source-tree relative path (development fallback)
 *
 * @param config Parsed config.
 * @return Resolved data directory path.
 * @version 1.8.1
 */
std::filesystem::path resolve_data_dir(const ParsedConfig& config)
{
    if (!config.config_dir.empty()) {
        auto dir = config.config_dir / "data";
        if (std::filesystem::is_directory(dir)) {
            s_log->info("Data dir from config: {}", dir.string());
            return dir;
        }
    }

    std::filesystem::path compile_time(CONFIG_ENTROPIC_DATA_DIR);
    if (std::filesystem::is_directory(compile_time)) {
        s_log->info("Data dir from compile-time path: {}",
                  compile_time.string());
        return compile_time;
    }

    // Development fallback: look relative to source tree.
    // This works when running from the build directory during development.
    std::filesystem::path src_data("src/entropic/data");
    if (std::filesystem::is_directory(src_data)) {
        s_log->info("Data dir from source tree: {}", src_data.string());
        return src_data;
    }

    // Last resort: data/ in current directory
    std::filesystem::path local_data("data");
    if (std::filesystem::is_directory(local_data)) {
        s_log->info("Data dir from local: {}", local_data.string());
        return local_data;
    }

    s_log->warn("No data directory found — bundled files unavailable");
    return "";
}

} // namespace entropic::config
