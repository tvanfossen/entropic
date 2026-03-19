/**
 * @file validate.h
 * @brief Config validation functions.
 *
 * Validation is separate from parsing. Parse populates structs,
 * validate checks cross-field constraints and range limits.
 * All functions return empty string on success, error on failure.
 *
 * @version 1.8.1
 */

#pragma once

#include <entropic/entropic_export.h>
#include <entropic/types/config.h>
#include <string>
#include <vector>

namespace entropic::config {

/**
 * @brief Validate a ModelConfig.
 *
 * Checks: context_length range [512, 131072], adapter non-empty,
 * allowed_tools entries use "server.tool" format.
 *
 * @param config Model config to validate.
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate(const ModelConfig& config);

/**
 * @brief Validate ModelsConfig.
 *
 * Checks: default tier exists in tiers dict.
 *
 * @param config Models config to validate.
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate(const ModelsConfig& config);

/**
 * @brief Validate CompactionConfig.
 *
 * Checks: warning_threshold_percent < threshold_percent,
 *         threshold_percent in [0.5, 0.99],
 *         preserve_recent_turns in [1, 10].
 *
 * @param config Compaction config to validate.
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate(const CompactionConfig& config);

/**
 * @brief Validate RoutingConfig against defined tiers.
 *
 * Cross-field: fallback_tier, tier_map values, handoff_rules
 * keys and values must all reference tiers that exist.
 *
 * @param routing Routing config.
 * @param models Models config (provides tier names).
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate_routing(
    const RoutingConfig& routing,
    const ModelsConfig& models);

/**
 * @brief Validate the full ParsedConfig.
 *
 * Runs all section validators + cross-section checks.
 *
 * @param config Full config to validate.
 * @param[out] warnings Non-fatal warnings (e.g., auto_chain without targets).
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate_config(
    const ParsedConfig& config,
    std::vector<std::string>& warnings);

/**
 * @brief Validate allowed_tools entries use "server.tool" format.
 * @param tools Tool name list to validate.
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate_allowed_tools(
    const std::vector<std::string>& tools);

/**
 * @brief Check fallback_tier exists in tiers.
 * @param fallback Fallback tier name.
 * @param tiers Defined tiers.
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate_fallback_tier(
    const std::string& fallback,
    const std::unordered_map<std::string, TierConfig>& tiers);

/**
 * @brief Check all tier_map values exist in tiers.
 * @param tier_map Classification to tier mapping.
 * @param tiers Defined tiers.
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate_tier_map(
    const std::unordered_map<std::string, std::string>& tier_map,
    const std::unordered_map<std::string, TierConfig>& tiers);

/**
 * @brief Check all handoff_rules keys and values exist in tiers.
 * @param rules Handoff rules.
 * @param tiers Defined tiers.
 * @return Empty string on success, error message on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate_handoff_rules(
    const std::unordered_map<std::string, std::vector<std::string>>& rules,
    const std::unordered_map<std::string, TierConfig>& tiers);

/**
 * @brief Warn if tier has auto_chain but no handoff_rules entry.
 * @param tiers Tier configs.
 * @param handoff_rules Handoff rules from routing config.
 * @return Warning message (empty if no issues).
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string warn_auto_chain_without_targets(
    const std::unordered_map<std::string, TierConfig>& tiers,
    const std::unordered_map<std::string, std::vector<std::string>>& handoff_rules);

} // namespace entropic::config
