/**
 * @file bundled_models.cpp
 * @brief BundledModels registry implementation.
 * @version 1.8.2
 */

#include <entropic/config/bundled_models.h>
#include <entropic/types/logging.h>
#include "yaml_util.h"

static auto s_log = entropic::log::get("config");

namespace entropic::config {

/**
 * @brief Load registry from YAML file.
 * @param path Path to bundled_models.yaml.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.8.2
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
 * @brief Resolve a model reference to a filesystem path.
 * @param value Registry key or direct path string.
 * @return Resolved filesystem path.
 * @internal
 * @version 1.8.2
 */
std::filesystem::path BundledModels::resolve(const std::string& value) const
{
    auto it = entries_.find(value);
    if (it != entries_.end()) {
        auto home = expand_home("~");
        return home / "models" / "gguf" / (it->second.name + ".gguf");
    }
    return expand_home(std::filesystem::path(value));
}

/**
 * @brief Get all entries.
 * @return Reference to the entries map.
 * @version 1.8.2
 */
const std::unordered_map<std::string, BundledModelEntry>&
BundledModels::entries() const
{
    return entries_;
}

} // namespace entropic::config
