/**
 * @file data_dir.cpp
 * @brief Bundled data directory resolution.
 * @version 2.0.5
 */

#include <entropic/config/loader.h>
#include <entropic/entropic_config.h>
#include <entropic/types/logging.h>

#include <cstdlib>
#include <vector>

static auto s_log = entropic::log::get("config");

namespace entropic::config {

/**
 * @brief Resolve the bundled data directory.
 *
 * Priority (v2.0.5):
 * 1. `ENTROPIC_DATA_DIR` env var (explicit operator override)
 * 2. `config.config_dir / "data"` (project-specific override)
 * 3. `CONFIG_ENTROPIC_DATA_DIR` (compile-time install path, typically
 *    `<prefix>/share/entropic`)
 * 4. `CONFIG_ENTROPIC_SOURCE_DATA_DIR` + CWD-relative `data/`
 *    (development fallback when running from the build tree)
 *
 * First path that `is_directory()` on disk wins.
 *
 * @param config Parsed config.
 * @return Resolved data directory path (empty if none found).
 * @internal
 * @version 2.0.5
 */
std::filesystem::path resolve_data_dir(const ParsedConfig& config)
{
    struct Candidate {
        std::filesystem::path path;
        const char* label;
    };

    std::vector<Candidate> candidates;

    // Priority 1: ENTROPIC_DATA_DIR env var (explicit operator override)
    if (const char* env = std::getenv("ENTROPIC_DATA_DIR"); env && *env) {
        candidates.push_back({env, "ENTROPIC_DATA_DIR env"});
    }

    // Priority 2: config.config_dir / "data"
    if (!config.config_dir.empty()) {
        candidates.push_back({config.config_dir / "data", "config.config_dir"});
    }

    // Priority 3-4: compile-time install path, then source-tree / CWD fallbacks
    candidates.push_back({CONFIG_ENTROPIC_DATA_DIR,        "compile-time install path"});
    candidates.push_back({CONFIG_ENTROPIC_SOURCE_DATA_DIR, "source tree (dev fallback)"});
    candidates.push_back({"data",                          "CWD-relative (dev fallback)"});

    std::filesystem::path result;
    for (const auto& [path, label] : candidates) {
        if (std::filesystem::is_directory(path)) {
            s_log->info("Data dir from {}: {}", label, path.string());
            result = path;
            break;
        }
    }

    if (result.empty()) {
        s_log->warn("No data directory found — bundled files unavailable");
    }
    return result;
}

} // namespace entropic::config
