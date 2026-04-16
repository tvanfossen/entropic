// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file data_dir.cpp
 * @brief Bundled data directory resolution.
 * @version 2.0.5.1
 */

#include <entropic/config/loader.h>
#include <entropic/entropic_config.h>
#include <entropic/types/logging.h>

#include <cstdlib>
#include <vector>

#include <dlfcn.h>

static auto s_log = entropic::log::get("config");

namespace entropic::config {

/**
 * @brief Locate `librentropic.so` on disk and derive its sibling share dir.
 *
 * The install layout is `<prefix>/lib/librentropic.so` +
 * `<prefix>/share/entropic/`. `dladdr()` on any symbol within
 * librentropic resolves to the .so's on-disk path, from which we can
 * compute `../share/entropic` relative to the library. This makes the
 * installed package portable: no absolute paths baked at build time,
 * no dependency on the consumer extracting to the same prefix the
 * packager built with.
 *
 * @return Resolved `<prefix>/share/entropic` path, or empty if dladdr
 *         fails (e.g. statically linked with no exported symbols).
 * @internal
 * @version 1
 */
static std::filesystem::path share_dir_from_library()
{
    // Take the address of resolve_data_dir itself — guaranteed to be
    // inside librentropic.so (we're in it). dladdr resolves this
    // pointer back to the .so's filesystem path.
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(&share_dir_from_library), &info) == 0
        || info.dli_fname == nullptr) {
        return {};
    }
    auto lib_path = std::filesystem::path(info.dli_fname);
    // dli_fname may be a relative path (e.g. just "librentropic.so")
    // when the loader resolved via LD_LIBRARY_PATH. Make it absolute
    // so ".." traversal is meaningful.
    std::error_code ec;
    auto abs_lib = std::filesystem::absolute(lib_path, ec);
    if (ec) {
        return {};
    }
    // <prefix>/lib/librentropic.so.2.0.5 → <prefix>/share/entropic
    return abs_lib.parent_path().parent_path() / "share" / "entropic";
}

/**
 * @brief Resolve the bundled data directory.
 *
 * Priority (v2.0.5.1):
 * 1. `ENTROPIC_DATA_DIR` env var (explicit operator override)
 * 2. `config.config_dir / "data"` (project-specific override)
 * 3. **Binary-relative discovery** via dladdr — `<prefix>/share/entropic`
 *    derived from librentropic.so's on-disk location. Portable across
 *    install prefixes regardless of build-time CMAKE_INSTALL_PREFIX.
 * 4. `CONFIG_ENTROPIC_DATA_DIR` (compile-time install path, last resort;
 *    historically unreliable for relocated installs).
 * 5. `CONFIG_ENTROPIC_SOURCE_DATA_DIR` + CWD-relative `data/`
 *    (development fallback when running from the build tree).
 *
 * First path that `is_directory()` on disk wins.
 *
 * @param config Parsed config.
 * @return Resolved data directory path (empty if none found).
 * @internal
 * @version 2.0.5.1
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

    // Priority 3: binary-relative discovery (portable across prefixes)
    if (auto from_lib = share_dir_from_library(); !from_lib.empty()) {
        candidates.push_back({from_lib, "binary-relative (dladdr)"});
    }

    // Priority 4-5: compile-time install path, then source-tree / CWD fallbacks
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
