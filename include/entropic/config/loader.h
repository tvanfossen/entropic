/**
 * @file loader.h
 * @brief Config loader — YAML to C++ structs with validation.
 *
 * Loads configuration using layered resolution:
 * 1. Compiled defaults (struct initializers)
 * 2. Global config (~/.entropic/config.yaml)
 * 3. Project config (.entropic/config.local.yaml)
 * 4. Environment variables (ENTROPIC_*)
 *
 * @version 1.8.1
 */

#pragma once

#include <entropic/entropic_export.h>
#include <entropic/types/config.h>
#include <entropic/config/bundled_models.h>
#include <filesystem>
#include <string>

namespace entropic::config {

/**
 * @brief Load config using layered resolution.
 *
 * Resolution order (highest wins):
 * 1. Compiled defaults (struct initializers)
 * 2. Global config (~/.entropic/config.yaml)
 * 3. Project config (.entropic/config.local.yaml)
 * 4. Environment variables (ENTROPIC_*)
 *
 * @param global_path Path to global config file (may not exist).
 * @param project_path Path to project config file (may not exist).
 * @param registry Bundled models registry for path resolution.
 * @param[out] config Output parsed config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string load_config(
    const std::filesystem::path& global_path,
    const std::filesystem::path& project_path,
    const BundledModels& registry,
    ParsedConfig& config);

/**
 * @brief Load config from a single YAML file (no layering).
 *
 * Applies compiled defaults first, then overlays the file.
 * Used by entropic_configure_from_file().
 *
 * @param path Path to YAML config file.
 * @param registry Bundled models registry for path resolution.
 * @param[out] config Output parsed config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string load_config_from_file(
    const std::filesystem::path& path,
    const BundledModels& registry,
    ParsedConfig& config);

/**
 * @brief Parse a config YAML file and overlay onto existing config.
 *
 * Fields not present in YAML retain their current values. This is
 * the merge primitive used for layered config loading.
 *
 * @param path Path to YAML file.
 * @param registry Bundled models for path resolution.
 * @param[in,out] config Config to overlay onto.
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string parse_config_file(
    const std::filesystem::path& path,
    const BundledModels& registry,
    ParsedConfig& config);

/**
 * @brief Apply ENTROPIC_* environment variable overrides.
 *
 * Variable format: ENTROPIC_{SECTION}__{FIELD} (double underscore).
 * Examples: ENTROPIC_LOG_LEVEL=DEBUG, ENTROPIC_ROUTING__ENABLED=true
 *
 * @param[in,out] config Config to override.
 * @version 1.8.1
 */
ENTROPIC_EXPORT void apply_env_overrides(ParsedConfig& config);

/**
 * @brief Resolve the bundled data directory.
 *
 * Priority:
 * 1. config.config_dir / "data" (if config_dir set)
 * 2. CONFIG_ENTROPIC_DATA_DIR (compile-time install path)
 * 3. Source-tree data/ (development fallback)
 *
 * @param config Parsed config.
 * @return Resolved data directory path.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::filesystem::path resolve_data_dir(const ParsedConfig& config);

/**
 * @brief Load config from a YAML/JSON string (no layering).
 *
 * Parses the string with ryml (accepts both YAML and JSON since
 * JSON is a YAML subset), applies env overrides, validates.
 * Used by entropic_configure().
 *
 * @param content Config string (YAML or JSON).
 * @param registry Bundled models registry for path resolution.
 * @param[out] config Output parsed config.
 * @return Empty string on success, error message on failure.
 * @version 2.0.0
 */
ENTROPIC_EXPORT std::string load_config_from_string(
    const std::string& content,
    const BundledModels& registry,
    ParsedConfig& config);

} // namespace entropic::config
