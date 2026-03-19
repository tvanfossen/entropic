/**
 * @file data_dir.cpp
 * @brief Bundled data directory resolution.
 * @version 1.8.2
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
 * @version 1.8.2
 */
std::filesystem::path resolve_data_dir(const ParsedConfig& config)
{
    struct Candidate {
        std::filesystem::path path;
        const char* label;
    };

    std::filesystem::path result;

    // Priority 1: config_dir/data
    if (!config.config_dir.empty()) {
        auto dir = config.config_dir / "data";
        if (std::filesystem::is_directory(dir)) {
            s_log->info("Data dir from config: {}", dir.string());
            result = dir;
        }
    }

    // Priority 2-4: compile-time, source tree, local
    if (result.empty()) {
        const Candidate candidates[] = {
            {CONFIG_ENTROPIC_DATA_DIR, "compile-time path"},
            {"src/entropic/data",      "source tree"},
            {"data",                   "local"},
        };

        for (const auto& [path, label] : candidates) {
            if (std::filesystem::is_directory(path)) {
                s_log->info("Data dir from {}: {}", label, path.string());
                result = path;
                break;
            }
        }
    }

    if (result.empty()) {
        s_log->warn("No data directory found — bundled files unavailable");
    }

    return result;
}

} // namespace entropic::config
