// SPDX-License-Identifier: Apache-2.0
/**
 * @file bundled_models.cpp
 * @brief BundledModels registry implementation.
 * @version 1.8.2
 */

#include <entropic/config/bundled_models.h>
#include <entropic/entropic_config.h>
#include <entropic/types/logging.h>
#include "yaml_util.h"

#include <cstdlib>
#include <vector>
#include <dlfcn.h>

static auto s_log = entropic::log::get("config");

namespace entropic::config {

namespace {

/**
 * @brief `<prefix>/share/entropic` derived from librentropic.so's
 *        on-disk location via dladdr.
 *
 * Mirrors data_dir.cpp::share_dir_from_library. Lets registry
 * discovery find the bundled_models.yaml relative to the installed
 * library, independent of the build-time CMAKE_INSTALL_PREFIX baked
 * into CONFIG_ENTROPIC_DATA_DIR. Without this a binary built with one
 * install prefix (CI/container stage dir, or a build host where the
 * stage dir is later cleaned) can't find its own registry once
 * installed elsewhere — every model key fails to resolve and tier
 * load dies with "Model file not found".
 *
 * @return Resolved share dir, or empty if dladdr fails.
 * @internal
 * @version 1.0
 */
std::filesystem::path share_dir_from_library()
{
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(&share_dir_from_library), &info) == 0
        || info.dli_fname == nullptr) {
        return {};
    }
    std::error_code ec;
    auto abs_lib = std::filesystem::absolute(
        std::filesystem::path(info.dli_fname), ec);
    if (ec) {
        return {};
    }
    return abs_lib.parent_path().parent_path() / "share" / "entropic";
}

}  // namespace

/**
 * @brief Load registry from YAML file.
 * @param path Path to bundled_models.yaml.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 2.1.8
 */
std::string BundledModels::load(const std::filesystem::path& path)
{
    std::string err;

    auto content = read_file(path);
    if (content.empty()) {
        err = "cannot read " + path.string();
    }

    ryml::Tree tree;
    ryml::ConstNodeRef root;
    if (err.empty()) {
        tree = ryml::parse_in_arena(
            ryml::to_csubstr(path.string()),
            ryml::to_csubstr(content));
        root = tree.rootref();

        if (!root.is_map()) {
            err = "bundled_models.yaml root is not a mapping";
        }
    }

    if (err.empty()) {
        for (auto child : root) {
            BundledModelEntry entry;
            entry.key = to_string(child.key());
            extract(child, "name", entry.name);
            extract(child, "url", entry.url);
            extract(child, "size_gb", entry.size_gb);
            extract(child, "adapter", entry.adapter);
            extract(child, "description", entry.description);
            extract(child, "mmproj_key", entry.mmproj_key);
            // gh#62 (v2.3.0): optional structured selectors. Entries
            // that don't declare them stay queryable by flat key only.
            extract(child, "provider",   entry.provider);
            extract(child, "family",     entry.family);
            extract(child, "size_label", entry.size_label);
            extract(child, "quant",      entry.quant);

            if (entry.name.empty()) {
                err = "bundled model '" + entry.key + "' missing 'name'";
                break;
            }

            s_log->info("Registered bundled model: {} -> {} ({:.1f} GB)",
                      entry.key, entry.name, entry.size_gb);
            entries_[entry.key] = std::move(entry);
        }
    }

    return err;
}

/**
 * @brief Check if a key exists in the registry.
 * @param key Registry key.
 * @return true if key exists.
 * @internal
 * @version 1.8.2
 */
bool BundledModels::contains(const std::string& key) const
{
    return entries_.count(key) > 0;
}

/**
 * @brief Get entry by key.
 * @param key Registry key.
 * @return Pointer to entry, or nullptr if not found.
 * @internal
 * @version 1.8.2
 */
const BundledModelEntry* BundledModels::get(const std::string& key) const
{
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return nullptr;
    }
    return &it->second;
}

/**
 * @brief gh#62: look up a flat key by (family, size_label, quant).
 *
 * Linear scan — entries_.size() is small (< 100 in practice). All
 * three selectors are required and matched exactly. Returns empty
 * string when no entry matches OR when the matching entries haven't
 * been backfilled with structured metadata yet.
 *
 * @internal
 * @version 2.3.0
 */
std::string BundledModels::find_by(
    const std::string& family,
    const std::string& size_label,
    const std::string& quant) const
{
    for (const auto& [key, entry] : entries_) {
        if (entry.family == family
            && entry.size_label == size_label
            && entry.quant == quant) {
            return key;
        }
    }
    return "";
}

/**
 * @brief Resolve a model reference to a filesystem path.
 *
 * If the argument is a known registry key, walk the v2.0.5 discovery
 * order:
 *
 *   1. `ENTROPIC_MODEL_DIR` env var — if set, wins unconditionally
 *      (operator intent), even if the file doesn't exist there yet.
 *      This lets the caller pre-specify a download destination.
 *   2. `~/.entropic/models/<name>.gguf` (user convention)
 *   3. `/opt/entropic/models/<name>.gguf` (system convention)
 *
 * For 2 and 3, first directory that actually contains `<name>.gguf`
 * wins; if neither exists on disk, #2 is returned so error messages
 * point at the most-likely-intended location.
 *
 * Otherwise the argument is treated as a direct path (with ~-expansion).
 *
 * @param value Registry key or direct path string.
 * @return Resolved filesystem path.
 * @internal
 * @version 2.0.5.1
 */
std::filesystem::path BundledModels::resolve(const std::string& value) const
{
    auto it = entries_.find(value);
    if (it == entries_.end()) {
        return expand_home(std::filesystem::path(value));
    }

    const auto filename = it->second.name + ".gguf";
    std::filesystem::path result;
    const char* reason = nullptr;

    // Priority 1: explicit operator override via env var always wins.
    // Deliberately does NOT check existence — the user may be pointing
    // at a download destination that hasn't been populated yet.
    if (const char* env = std::getenv("ENTROPIC_MODEL_DIR"); env && *env) {
        result = std::filesystem::path(env) / filename;
        reason = "ENTROPIC_MODEL_DIR";
    } else {
        // Priority 2-3: user home, then system. First existing file wins;
        // if neither exists, home_path is returned so error messages
        // point at the most-likely-intended location.
        const auto home_path =
            expand_home("~") / ".entropic" / "models" / filename;
        const auto sys_path =
            std::filesystem::path("/opt/entropic/models") / filename;
        if (std::filesystem::is_regular_file(sys_path)
            && !std::filesystem::is_regular_file(home_path)) {
            result = sys_path;
            reason = "/opt/entropic/models";
        } else {
            result = home_path;
            reason = std::filesystem::is_regular_file(home_path)
                ? "~/.entropic/models"
                : "fallback (file not found)";
        }
    }

    s_log->info("Model '{}' resolved to {} ({})",
                value, result.string(), reason);
    return result;
}

/**
 * @brief Get all entries.
 * @return Reference to the entries map.
 * @utility
 * @version 1.8.2
 */
const std::unordered_map<std::string, BundledModelEntry>&
BundledModels::entries() const
{
    return entries_;
}

/**
 * @brief Auto-discover and load bundled_models.yaml.
 *
 * Discovery order (mirrors data_dir.cpp::resolve_data_dir):
 * ENTROPIC_DATA_DIR env, then binary-relative via dladdr
 * (<prefix>/share/entropic from librentropic.so's location), then the
 * compile-time install path, source tree, and CWD-relative "data/"
 * fallbacks. The dladdr step makes the registry findable on any
 * install prefix, not just the build host.
 *
 * @return Empty string on success, error message if none found.
 * @internal
 * @version 2.3.6
 */
std::string BundledModels::auto_discover_and_load() {
    // Discovery order mirrors data_dir.cpp::resolve_data_dir so the
    // registry is found the same way as prompts/grammars/schemas:
    //   1. ENTROPIC_DATA_DIR env (explicit operator override)
    //   2. binary-relative via dladdr (portable across install prefixes)
    //   3-5. compile-time install path, source tree, CWD (dev/build-host)
    // Pre-v2.3.6 this list was only 3-5 — purely compile-time/CWD — so
    // a relocated install (any machine that isn't the build host, incl.
    // container-staged release binaries) silently loaded zero models.
    std::vector<std::filesystem::path> candidates;
    if (const char* env = std::getenv("ENTROPIC_DATA_DIR"); env && *env) {
        candidates.emplace_back(std::filesystem::path(env) / "bundled_models.yaml");
    }
    if (auto from_lib = share_dir_from_library(); !from_lib.empty()) {
        candidates.emplace_back(from_lib / "bundled_models.yaml");
    }
    candidates.emplace_back(
        std::filesystem::path(CONFIG_ENTROPIC_DATA_DIR) / "bundled_models.yaml");
    candidates.emplace_back(
        std::filesystem::path(CONFIG_ENTROPIC_SOURCE_DATA_DIR) / "bundled_models.yaml");
    candidates.emplace_back("data/bundled_models.yaml");

    for (const auto& path : candidates) {
        if (std::filesystem::is_regular_file(path)) {
            auto err = load(path);
            if (err.empty()) {
                s_log->info("pre-loaded {} bundled model(s) from {}",
                            entries_.size(), path.string());
                return "";
            }
            s_log->warn("bundled models load failed: {} — {}", path.string(), err);
        }
    }
    return "bundled_models.yaml not found in default paths";
}

} // namespace entropic::config
