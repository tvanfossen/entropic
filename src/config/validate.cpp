// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file validate.cpp
 * @brief Config validation implementation.
 * @version 1.8.2
 */

#include <entropic/config/validate.h>
#include <entropic/types/logging.h>

static auto s_log = entropic::log::get("config");

namespace entropic::config {

/**
 * @brief Validate allowed_tools entries use "server.tool" format.
 * @param tools Tool name list to validate.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.8.2
 */
std::string validate_allowed_tools(const std::vector<std::string>& tools)
{
    for (const auto& entry : tools) {
        if (entry.find('.') == std::string::npos) {
            return "allowed_tools entry '" + entry
                   + "' must use '{server_name}.{tool_name}' format";
        }
    }
    return "";
}

/**
 * @brief Validate a ModelConfig.
 * @param config Model config to validate.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.8.2
 */
std::string validate(const ModelConfig& config)
{
    std::string err;

    if (config.context_length < 512 || config.context_length > 131072) {
        err = "context_length must be in [512, 131072], got "
              + std::to_string(config.context_length);
    } else if (config.adapter.empty()) {
        err = "adapter must not be empty";
    } else if (config.n_batch < 1) {
        err = "n_batch must be >= 1, got "
              + std::to_string(config.n_batch);
    } else if (config.allowed_tools.has_value()) {
        err = validate_allowed_tools(*config.allowed_tools);
    }

    return err;
}

/**
 * @brief Validate ModelsConfig.
 * @param config Models config to validate.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.8.2
 */
std::string validate(const ModelsConfig& config)
{
    if (!config.tiers.empty()
        && config.tiers.count(config.default_tier) == 0) {
        return "default tier '" + config.default_tier
               + "' not in tiers";
    }
    return "";
}

/**
 * @brief Validate CompactionConfig.
 * @param config Compaction config to validate.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 2.1.3
 */
std::string validate(const CompactionConfig& config)
{
    std::string err;

    if (config.threshold_percent < 0.5f
        || config.threshold_percent > 0.99f) {
        err = "compaction.threshold_percent must be in [0.5, 0.99], got "
              + std::to_string(config.threshold_percent);
    } else if (config.warning_threshold_percent
               >= config.threshold_percent) {
        err = "compaction.warning_threshold_percent ("
              + std::to_string(config.warning_threshold_percent)
              + ") must be less than threshold_percent ("
              + std::to_string(config.threshold_percent) + ")";
    } else if (config.preserve_recent_turns < 1
               || config.preserve_recent_turns > 10) {
        err = "compaction.preserve_recent_turns must be in [1, 10], got "
              + std::to_string(config.preserve_recent_turns);
    } else if (config.tool_result_ttl < 1) {
        // v2.1.3 #6 / TTL clamp removal: tool_result_ttl was previously
        // documented as "1–20" but never actually validated. The upper
        // bound was always advisory — consumers can pick whatever
        // makes sense for their workload (long delegations on large
        // context windows benefit from large TTLs). Lower bound IS
        // enforced because TTL=0 would prune every result on the next
        // iteration, which is nonsensical and almost certainly a
        // misconfiguration rather than an intent.
        err = "compaction.tool_result_ttl must be >= 1, got "
              + std::to_string(config.tool_result_ttl);
    }

    return err;
}

/**
 * @brief Check fallback_tier exists in tiers.
 * @param fallback Fallback tier name.
 * @param tiers Defined tiers.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 * @utility
 */
std::string validate_fallback_tier(
    const std::string& fallback,
    const std::unordered_map<std::string, TierConfig>& tiers)
{
    if (tiers.count(fallback) == 0) {
        return "routing.fallback_tier '" + fallback + "' not in tiers";
    }
    return "";
}

/**
 * @brief Check all tier_map values exist in tiers.
 * @param tier_map Classification to tier mapping.
 * @param tiers Defined tiers.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 * @utility
 */
std::string validate_tier_map(
    const std::unordered_map<std::string, std::string>& tier_map,
    const std::unordered_map<std::string, TierConfig>& tiers)
{
    for (const auto& [key, tier] : tier_map) {
        if (tiers.count(tier) == 0) {
            return "routing.tier_map['" + key + "'] = '" + tier
                   + "' not in tiers";
        }
    }
    return "";
}

/**
 * @brief Check all handoff_rules keys and values exist in tiers.
 * @param rules Handoff rules.
 * @param tiers Defined tiers.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 * @utility
 */
std::string validate_handoff_rules(
    const std::unordered_map<std::string, std::vector<std::string>>& rules,
    const std::unordered_map<std::string, TierConfig>& tiers)
{
    for (const auto& [source, targets] : rules) {
        if (tiers.count(source) == 0) {
            return "routing.handoff_rules key '" + source
                   + "' not in tiers";
        }
        for (const auto& target : targets) {
            if (tiers.count(target) == 0) {
                return "routing.handoff_rules['" + source
                       + "'] contains '" + target + "' not in tiers";
            }
        }
    }
    return "";
}

/**
 * @brief Validate RoutingConfig against defined tiers.
 * @param routing Routing config.
 * @param models Models config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 * @utility
 */
std::string validate_routing(
    const RoutingConfig& routing,
    const ModelsConfig& models)
{
    std::string err;

    if (models.tiers.empty()) {
        /* no tiers — nothing to validate */
    } else if (routing.enabled && !models.router.has_value()) {
        err = "routing.enabled is true but models.router is not configured";
    } else {
        err = validate_fallback_tier(routing.fallback_tier, models.tiers);

        if (err.empty()) {
            err = validate_tier_map(routing.tier_map, models.tiers);
        }
        if (err.empty()) {
            err = validate_handoff_rules(routing.handoff_rules, models.tiers);
        }
    }

    return err;
}

/**
 * @brief Warn if tier has auto_chain but no handoff_rules entry.
 * @param tiers Tier configs.
 * @param handoff_rules Handoff rules.
 * @return Warning message (empty if no issues).
 * @internal
 * @version 1.8.2
 */
std::string warn_auto_chain_without_targets(
    const std::unordered_map<std::string, TierConfig>& tiers,
    const std::unordered_map<std::string, std::vector<std::string>>&
        handoff_rules)
{
    std::string warnings;
    for (const auto& [name, tier] : tiers) {
        if (tier.auto_chain.has_value() && !tier.auto_chain->empty()
            && handoff_rules.count(name) == 0) {
            if (!warnings.empty()) {
                warnings += "; ";
            }
            warnings += "tier '" + name
                        + "' has auto_chain but no handoff_rules entry";
        }
    }
    return warnings;
}

/**
 * @brief Validate PromptCacheConfig.
 * @param config Prompt cache config to validate.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.8.3
 */
std::string validate(const PromptCacheConfig& config)
{
    if (config.enabled && config.max_bytes == 0) {
        return "inference.prompt_cache: enabled=true but max_bytes=0";
    }
    return "";
}

/**
 * @brief Validate model tiers and router.
 * @param models Models config.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.8.3
 */
static std::string validate_model_tiers(const ModelsConfig& models)
{
    std::string err = validate(models);

    if (err.empty()) {
        for (const auto& [name, tier] : models.tiers) {
            err = validate(static_cast<const ModelConfig&>(tier));
            if (!err.empty()) {
                err = "models." + name + ": " + err;
                break;
            }
        }
    }

    if (err.empty() && models.router.has_value()) {
        err = validate(*models.router);
        if (!err.empty()) {
            err = "models.router: " + err;
        }
    }

    return err;
}

/**
 * @brief Validate the full ParsedConfig.
 * @param config Full config to validate.
 * @param[out] warnings Non-fatal warnings.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 2.0.0
 */
std::string validate_config(
    const ParsedConfig& config,
    std::vector<std::string>& warnings)
{
    s_log->info("Config validation start");
    std::string err = validate_model_tiers(config.models);

    if (err.empty()) {
        err = validate_routing(config.routing, config.models);
    }
    if (err.empty()) {
        err = validate(config.compaction);
    }
    if (err.empty()) {
        err = validate(config.prompt_cache);
    }
    if (err.empty()) {
        auto w = warn_auto_chain_without_targets(
            config.models.tiers, config.routing.handoff_rules);
        if (!w.empty()) {
            warnings.push_back(w);
        }
    }

    if (err.empty()) {
        s_log->info("Config validation passed ({} warning(s))",
                    warnings.size());
    } else {
        s_log->error("Config validation failed: {}", err);
    }
    return err;
}

} // namespace entropic::config
