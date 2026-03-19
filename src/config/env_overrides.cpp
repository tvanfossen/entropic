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
 * @internal
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
 * @brief Apply ENTROPIC_* environment variable overrides.
 * @param[in,out] config Config to override.
 * @version 1.8.1
 * @utility
 */
void apply_env_overrides(ParsedConfig& config)
{
    auto val = get_env("ENTROPIC_LOG_LEVEL");
    if (!val.empty()) {
        s_log->info("Env override: ENTROPIC_LOG_LEVEL={}", val);
        config.log_level = val;
    }

    val = get_env("ENTROPIC_MODELS__DEFAULT");
    if (!val.empty()) {
        s_log->info("Env override: ENTROPIC_MODELS__DEFAULT={}", val);
        config.models.default_tier = val;
    }

    val = get_env("ENTROPIC_ROUTING__ENABLED");
    if (!val.empty()) {
        s_log->info("Env override: ENTROPIC_ROUTING__ENABLED={}", val);
        config.routing.enabled = parse_bool(val);
    }

    val = get_env("ENTROPIC_ROUTING__FALLBACK_TIER");
    if (!val.empty()) {
        s_log->info("Env override: ENTROPIC_ROUTING__FALLBACK_TIER={}", val);
        config.routing.fallback_tier = val;
    }

    val = get_env("ENTROPIC_COMPACTION__THRESHOLD_PERCENT");
    if (!val.empty()) {
        s_log->info("Env override: ENTROPIC_COMPACTION__THRESHOLD_PERCENT={}",
                  val);
        config.compaction.threshold_percent = std::stof(val);
    }

    val = get_env("ENTROPIC_COMPACTION__ENABLED");
    if (!val.empty()) {
        s_log->info("Env override: ENTROPIC_COMPACTION__ENABLED={}", val);
        config.compaction.enabled = parse_bool(val);
    }

    val = get_env("ENTROPIC_VRAM_RESERVE_MB");
    if (!val.empty()) {
        s_log->info("Env override: ENTROPIC_VRAM_RESERVE_MB={}", val);
        config.vram_reserve_mb = std::stoi(val);
    }

    val = get_env("ENTROPIC_CONFIG_DIR");
    if (!val.empty()) {
        s_log->info("Env override: ENTROPIC_CONFIG_DIR={}", val);
        config.config_dir = std::filesystem::path(val);
    }
}

} // namespace entropic::config
