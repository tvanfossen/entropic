/**
 * @file bundled_models.cpp
 * @brief BundledModels registry implementation.
 * @version 1.8.1
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
 * @version 1.8.1
 */
std::string BundledModels::load(const std::filesystem::path& path)
{
    auto content = read_file(path);
    if (content.empty()) {
        return "cannot read " + path.string();
    }

    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr(path.string()),
        ryml::to_csubstr(content));
    ryml::ConstNodeRef root = tree.rootref();

    if (!root.is_map()) {
        return "bundled_models.yaml root is not a mapping";
    }

    for (auto child : root) {
        BundledModelEntry entry;
        entry.key = to_string(child.key());
        extract(child, "name", entry.name);
        extract(child, "url", entry.url);
        extract(child, "size_gb", entry.size_gb);
        extract(child, "adapter", entry.adapter);
        extract(child, "description", entry.description);

        if (entry.name.empty()) {
            return "bundled model '" + entry.key + "' missing 'name'";
        }

        s_log->info("Registered bundled model: {} -> {} ({:.1f} GB)",
                  entry.key, entry.name, entry.size_gb);
        entries_[entry.key] = std::move(entry);
    }

    return "";
}

/**
 * @brief Check if a key exists in the registry.
 * @param key Registry key.
 * @return true if key exists.
 * @version 1.8.1
 */
bool BundledModels::contains(const std::string& key) const
{
    return entries_.count(key) > 0;
}

/**
 * @brief Get entry by key.
 * @param key Registry key.
 * @return Pointer to entry, or nullptr if not found.
 * @version 1.8.1
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
 * @version 1.8.1
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
 * @version 1.8.1
 */
const std::unordered_map<std::string, BundledModelEntry>&
BundledModels::entries() const
{
    return entries_;
}

} // namespace entropic::config
