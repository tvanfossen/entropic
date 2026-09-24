// SPDX-License-Identifier: Apache-2.0
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
 * @req REQ-CFG-006
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
 * @req REQ-CFG-006
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
 * @req REQ-CFG-006
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
 * @req REQ-CFG-006
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate_routing(
    const RoutingConfig& routing,
    const ModelsConfig& models);

/**
 * @brief Validate PromptCacheConfig.
 *
 * Checks: max_bytes > 0 when enabled.
 *
 * @param config Prompt cache config to validate.
 * @return Empty string on success, error message on failure.
 * @req REQ-CFG-006
 * @version 1.8.3
 */
ENTROPIC_EXPORT std::string validate(const PromptCacheConfig& config);

/**
 * @brief Validate the full ParsedConfig.
 *
 * Runs all section validators + cross-section checks.
 *
 * @param config Full config to validate.
 * @param[out] warnings Non-fatal warnings (e.g., auto_chain without targets).
 * @return Empty string on success, error message on failure.
 * @req REQ-CFG-006
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate_config(
    const ParsedConfig& config,
    std::vector<std::string>& warnings);

/**
 * @brief Validate allowed_tools entries use "server.tool" format.
 * @param tools Tool name list to validate.
 * @return Empty string on success, error message on failure.
 * @req REQ-CFG-006
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string validate_allowed_tools(
    const std::vector<std::string>& tools);

/**
 * @brief Check fallback_tier exists in tiers.
 * @param fallback Fallback tier name.
 * @param tiers Defined tiers.
 * @return Empty string on success, error message on failure.
 * @req REQ-CFG-006
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
 * @req REQ-CFG-006
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
 * @req REQ-CFG-006
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
 * @req REQ-CFG-006
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string warn_auto_chain_without_targets(
    const std::unordered_map<std::string, TierConfig>& tiers,
    const std::unordered_map<std::string, std::vector<std::string>>& handoff_rules);

/**
 * @brief The directories a bare grammar stem is looked up in (gh#154).
 *
 * Mirrors the runtime rule rather than inventing one:
 * `ModelOrchestrator::load_bundled_grammars` reads
 * `<config_dir>/grammars`, and the facade falls back to
 * `<data_dir>/grammars` only when that produced nothing. Both are
 * returned when the first holds no `.gbnf`, so the check accepts exactly
 * what the registry will hold.
 *
 * @param config Parsed engine config (supplies `config_dir`).
 * @param data_dir Resolved bundled data directory.
 * @return Search directories, most specific first. May be empty.
 * @req REQ-INFER-007
 * @version 2.13.0
 */
ENTROPIC_EXPORT std::vector<std::filesystem::path> grammar_search_paths(
    const ParsedConfig& config,
    const std::filesystem::path& data_dir);

/**
 * @brief Warn about a tier whose `grammar:` stem resolves to no .gbnf
 *        (gh#154).
 *
 * Names a documented fail-open early. `GrammarRegistry::get()` returns ""
 * on a miss and the decode proceeds UNCONSTRAINED — which is undetectable
 * from outside, because constrained and unconstrained output have the
 * same shape whenever the prompt also describes the shape. A consumer
 * measured speculative decode for three days believing a grammar was
 * active; their harness wrote each arm's config where no matching
 * `.gbnf` sat.
 *
 * A WARNING and not an error, because at configure time "unresolved" does
 * not yet mean "wrong". `entropic_grammar_register` and
 * `entropic_grammar_register_file` both require an orchestrator, which
 * exists only AFTER `entropic_configure*`, so registering a tier's grammar
 * from the host application is necessarily a post-configure act. gh#154
 * first made this an error and thereby made that documented sequence
 * impossible to perform; the refusal now lands at FIRST USE instead
 * (`ModelOrchestrator::refuse_unresolved_tier_grammar`, which returns
 * `ENTROPIC_ERROR_GRAMMAR_NOT_FOUND` and decodes nothing).
 *
 * A runtime `params.grammar_key` is not checked at all: it is per-call and
 * keeps its fail-open, reported through `generations[].grammar.resolved`.
 *
 * @param config Parsed engine config (tiers carry the stems).
 * @param search_dirs Directories from grammar_search_paths().
 * @return Empty string when every tier stem resolves, else one message
 *         naming EVERY unresolved tier, its stem, every directory
 *         searched, and how to register the grammar.
 * @req REQ-INFER-007
 * @req REQ-CFG-006
 * @version 2.13.0
 */
ENTROPIC_EXPORT std::string warn_unresolved_tier_grammars(
    const ParsedConfig& config,
    const std::vector<std::filesystem::path>& search_dirs);

} // namespace entropic::config
