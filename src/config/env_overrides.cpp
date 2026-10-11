// SPDX-License-Identifier: Apache-2.0
/**
 * @file env_overrides.cpp
 * @brief ENTROPIC_* environment variable override implementation.
 * @version 1.8.1
 */

#include <entropic/config/loader.h>
#include <entropic/types/logging.h>
#include <cstdlib>

static auto s_log = entropic::log::get("config");

namespace entropic::config {

/**
 * @brief Get environment variable value or empty string.
 * @param name Variable name.
 * @return Value string, or empty if not set.
 * @dg_internal
 * @version 1.8.1
 */
static std::string get_env(const char* name)
{
    const char* val = std::getenv(name);
    return val != nullptr ? std::string(val) : "";
}

/**
 * @brief Parse a boolean from string (true/false/yes/no/1/0).
 * @param val String value.
 * @return Parsed boolean.
 * @version 1.8.1
 * @utility
 */
static bool parse_bool(const std::string& val)
{
    return val == "true" || val == "True" || val == "TRUE"
           || val == "yes" || val == "1";
}

/**
 * @brief Apply one ENTROPIC_* override if the env var is set.
 *
 * Reads `key`; on a non-empty value logs it and hands it to `setter`.
 * Collapses the repeated get/check/log/assign block so the dispatcher
 * stays knots-clean.
 *
 * @param key Environment variable name.
 * @param setter Invoked with the value when present.
 * @utility
 * @version 2.3.7
 */
template <typename Setter>
static void apply_env(const char* key, Setter setter) {
    auto val = get_env(key);
    if (val.empty()) { return; }
    s_log->info("Env override: {}={}", key, val);
    setter(val);
}

/**
 * @brief The `mcp.external` overrides `entropic serve` sets (gh#204).
 *
 * `ENTROPIC_MCP__EXTERNAL__ENABLED` and `ENTROPIC_MCP__EXTERNAL__HTTP_PORT`.
 * A port that does not parse becomes -1, which the HTTP listener then
 * refuses loudly at configure — never an exception out of the loader.
 *
 * @param config Config to override.
 * @dg_internal
 * @req REQ-CFG-001
 * @version 2.14.0
 */
static void apply_mcp_external_env(ParsedConfig& config)
{
    apply_env("ENTROPIC_MCP__EXTERNAL__ENABLED",
              [&](const std::string& v) {
                  config.mcp.external.enabled = parse_bool(v);
              });
    apply_env("ENTROPIC_MCP__EXTERNAL__HTTP_PORT",
              [&](const std::string& v) {
                  char* end = nullptr;
                  const long port = std::strtol(v.c_str(), &end, 10);
                  config.mcp.external.http_port =
                      (end != v.c_str() && *end == '\0') ? static_cast<int>(port)
                                                          : -1;
              });
}

/**
 * @brief Apply ENTROPIC_* environment variable overrides.
 * @param[in,out] config Config to override.
 * @version 2.14.0
 * @req REQ-CFG-001
 */
void apply_env_overrides(ParsedConfig& config)
{
    apply_env("ENTROPIC_LOG_LEVEL",
              [&](const std::string& v) { config.log_level = v; });
    apply_env("ENTROPIC_MODELS__DEFAULT",
              [&](const std::string& v) { config.models.default_tier = v; });
    apply_env("ENTROPIC_ROUTING__ENABLED",
              [&](const std::string& v) {
                  config.routing.enabled = parse_bool(v);
              });
    apply_env("ENTROPIC_ROUTING__FALLBACK_TIER",
              [&](const std::string& v) { config.routing.fallback_tier = v; });
    apply_env("ENTROPIC_COMPACTION__THRESHOLD_PERCENT",
              [&](const std::string& v) {
                  config.compaction.threshold_percent = std::stof(v);
              });
    apply_env("ENTROPIC_COMPACTION__ENABLED",
              [&](const std::string& v) {
                  config.compaction.enabled = parse_bool(v);
              });
    apply_env("ENTROPIC_VRAM_RESERVE_MB",
              [&](const std::string& v) {
                  config.vram_reserve_mb = std::stoi(v);
              });
    apply_env("ENTROPIC_CONFIG_DIR",
              [&](const std::string& v) {
                  config.config_dir = std::filesystem::path(v);
              });
    apply_mcp_external_env(config);  // gh#204 (v2.14.0)
}

} // namespace entropic::config
