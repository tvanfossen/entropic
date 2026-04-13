/**
 * @file bundled_models.h
 * @brief Bundled model registry — resolves keys to filesystem paths.
 *
 * Loaded from bundled_models.yaml. Maps short keys (e.g., "primary")
 * to model names, URLs, and metadata. Used during config loading to
 * resolve model path fields.
 *
 * @version 1.8.1
 */

#pragma once

#include <entropic/entropic_export.h>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace entropic::config {

/**
 * @brief Entry in the bundled model registry.
 *
 * Maps a key (e.g., "primary") to a model name, URL, and metadata.
 * Parsed from bundled_models.yaml.
 *
 * @version 1.8.1
 */
struct BundledModelEntry {
    std::string key;          ///< Registry key (e.g., "primary")
    std::string name;         ///< Model filename stem (e.g., "Qwen3.5-35B-A3B-UD-IQ3_XXS")
    std::string url;          ///< Download URL
    double size_gb = 0.0;     ///< Model size in GB
    std::string adapter;      ///< Adapter name (e.g., "qwen35")
    std::string description;  ///< Human-readable description
};

/**
 * @brief Bundled model registry loaded from bundled_models.yaml.
 *
 * @par Resolution
 * When a config field contains a registry key (e.g., "primary"),
 * resolve() returns ~/models/gguf/{name}.gguf. When the value is
 * already a path, it passes through with ~ expansion.
 *
 * @par Thread safety
 * Immutable after load. Safe to read from any thread.
 *
 * @return A default-constructed BundledModels instance; populate via load().
 * @req REQ-CFG-003
 * @version 2.0.2
 */
class ENTROPIC_EXPORT BundledModels {
public:
    /**
     * @brief Load registry from YAML file.
     * @param path Path to bundled_models.yaml.
     * @return Empty string on success, error message on failure.
     * @version 1.8.1
     */
    std::string load(const std::filesystem::path& path);

    /**
     * @brief Check if a key exists in the registry.
     * @param key Registry key.
     * @return true if key exists.
     * @version 1.8.1
     */
    bool contains(const std::string& key) const;

    /**
     * @brief Get entry by key.
     * @param key Registry key.
     * @return Pointer to entry, or nullptr if not found.
     * @version 1.8.1
     */
    const BundledModelEntry* get(const std::string& key) const;

    /**
     * @brief Resolve a model reference to a filesystem path.
     *
     * If value matches a registry key, returns ~/models/gguf/{name}.gguf.
     * Otherwise treats value as a direct path with ~ expansion.
     *
     * @param value Registry key or direct path string.
     * @return Resolved filesystem path.
     * @version 1.8.1
     */
    std::filesystem::path resolve(const std::string& value) const;

    /**
     * @brief Get all entries.
     * @return Reference to the entries map.
     * @version 1.8.1
     */
    const std::unordered_map<std::string, BundledModelEntry>& entries() const;

    /**
     * @brief Auto-discover and load bundled_models.yaml.
     *
     * Searches compile-time install path, source tree path, and
     * CWD-relative "data/" for bundled_models.yaml. Loads the first
     * one found.
     *
     * @return Empty string on success, error message if not found.
     * @version 2.0.1
     */
    std::string auto_discover_and_load();

private:
    std::unordered_map<std::string, BundledModelEntry> entries_; ///< Key → entry map
};

} // namespace entropic::config
