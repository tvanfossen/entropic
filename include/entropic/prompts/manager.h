// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file manager.h
 * @brief Prompt manager — frontmatter parsing, identity loading, assembly.
 *
 * Ports Python's PromptManager and parse_prompt_file(). Handles
 * constitution, app_context, and per-tier identity prompt files.
 *
 * @version 1.8.1
 */

#pragma once

#include <entropic/entropic_export.h>
#include <entropic/types/config.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic::prompts {

/// @brief Alias for PhaseConfig from types/ (canonical location).
/// @version 1.9.6
using PhaseConfig = entropic::PhaseConfig;

/**
 * @brief Prompt file type (frontmatter "type" field).
 * @version 1.8.1
 */
enum class PromptType {
    CONSTITUTION,  ///< Constitution prompt
    APP_CONTEXT,   ///< Application context prompt
    IDENTITY,      ///< Tier identity prompt
};

/**
 * @brief A single benchmark prompt with quality checks.
 * @version 1.8.1
 */
struct BenchmarkPrompt {
    std::string prompt;                      ///< Prompt text
    std::vector<std::string> checks_yaml;    ///< Check defs as YAML strings
};

/**
 * @brief Benchmark definition for an identity.
 * @version 1.8.1
 */
struct BenchmarkSpec {
    std::vector<BenchmarkPrompt> prompts; ///< Benchmark prompts
};

/**
 * @brief Identity frontmatter — full tier identity metadata.
 *
 * Maps to Python's IdentityFrontmatter. Inference behavior params
 * live here (not in ModelConfig). Config contains hardware/load-time
 * params only.
 *
 * @version 1.8.1
 */
struct IdentityFrontmatter {
    PromptType type = PromptType::IDENTITY;    ///< Always IDENTITY
    int version = 1;                           ///< Schema version

    std::string name;                          ///< Tier name (e.g., "lead")
    std::vector<std::string> focus;            ///< Focus areas (min 1)
    std::vector<std::string> examples;         ///< Few-shot examples
    std::optional<std::string> grammar;        ///< Grammar file reference
    std::optional<std::string> auto_chain;     ///< Auto-chain target tier
    std::optional<std::vector<std::string>> allowed_tools; ///< Tool filter
    std::optional<std::vector<std::string>> bash_commands;  ///< Allowed bash commands
    int max_output_tokens = 1024;              ///< Default max output tokens
    float temperature = 0.7f;                  ///< Default temperature
    float repeat_penalty = 1.1f;               ///< Default repetition penalty
    bool enable_thinking = false;              ///< Default thinking mode
    std::string model_preference = "primary";  ///< Model preference key
    bool interstitial = false;                 ///< Interstitial role
    bool routable = true;                      ///< Visible to router
    std::string role_type = "front_office";    ///< front_office|back_office|utility
    bool explicit_completion = false;          ///< Requires explicit completion
    std::vector<std::string> validation_rules; ///< Per-identity constitutional rules (v2.0.6)
    bool relay_single_delegate = false;        ///< Skip re-synthesis when single delegate returns (v2.0.11)
    std::optional<std::unordered_map<std::string, PhaseConfig>> phases; ///< Named phases
    std::optional<BenchmarkSpec> benchmark;    ///< Benchmark definition
};

/**
 * @brief Parsed prompt file result: type + version + body.
 * @version 1.8.1
 */
struct ParsedPrompt {
    PromptType type;       ///< Prompt type
    int version = 1;       ///< Schema version
    std::string body;      ///< Markdown body after frontmatter
};

/**
 * @brief Parsed identity file: frontmatter + body.
 * @version 1.8.1
 */
struct ParsedIdentity {
    IdentityFrontmatter frontmatter; ///< Full identity metadata
    std::string body;                ///< Markdown system prompt body
};

/**
 * @brief Convert PromptType to string.
 * @param type Prompt type.
 * @return String representation.
 * @version 1.8.1
 */
ENTROPIC_EXPORT const char* prompt_type_to_string(PromptType type);

/**
 * @brief Parse a prompt file: validate frontmatter, return body.
 *
 * File format: YAML frontmatter between --- delimiters, followed by
 * markdown body.
 *
 * @param path Path to .md prompt file.
 * @param expected_type Expected frontmatter type.
 * @param[out] result Output: type, version, body.
 * @return Empty string on success, error on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string parse_prompt_file(
    const std::filesystem::path& path,
    PromptType expected_type,
    ParsedPrompt& result);

/**
 * @brief Load an identity file: parse frontmatter + body.
 *
 * Convenience wrapper that additionally parses all
 * IdentityFrontmatter fields.
 *
 * @param path Path to identity .md file.
 * @param[out] identity Output parsed identity.
 * @return Empty string on success, error on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string load_identity(
    const std::filesystem::path& path,
    ParsedIdentity& identity);

/**
 * @brief Load constitution prompt with tri-state resolution.
 *
 * @param constitution_path Custom path (nullopt = bundled).
 * @param disabled true if constitution explicitly disabled.
 * @param data_dir Bundled data directory for fallback.
 * @param[out] body Output constitution text (empty if disabled).
 * @return Empty string on success, error on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string load_constitution(
    const std::optional<std::filesystem::path>& constitution_path,
    bool disabled,
    const std::filesystem::path& data_dir,
    std::string& body);

/**
 * @brief Load app_context prompt with tri-state resolution.
 *
 * Resolution order: if disabled or app_context_path is nullopt, body
 * is left empty (no bundled fallback — app_context is opt-in). If a
 * path is provided and is a bare filename, it is resolved relative
 * to data_dir/prompts/. Absolute paths and paths with directory
 * components are used as-is.
 *
 * @param app_context_path Custom path (nullopt = disabled by default).
 * @param disabled true if app_context explicitly disabled.
 * @param data_dir Bundled data directory used to resolve bare filenames.
 * @param[out] body Output app_context text (empty if disabled or nullopt).
 * @return Empty string on success, error on failure.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string load_app_context(
    const std::optional<std::filesystem::path>& app_context_path,
    bool disabled,
    const std::filesystem::path& data_dir,
    std::string& body);

/**
 * @brief Resolve the system prompt body for a named tier.
 *
 * Looks up the tier in config, resolves the identity file path
 * (explicit, bundled convention, or disabled), loads it, and
 * returns the markdown body. Encapsulates the path convention
 * "identity_{tier_name}.md" in one place.
 *
 * @param tier_config Tier configuration.
 * @param tier_name Tier name (for default path convention).
 * @param data_dir Bundled data directory.
 * @return Identity body string (empty if disabled or not found).
 * @version 2.0.1
 */
ENTROPIC_EXPORT std::string resolve_tier_identity(
    const entropic::TierConfig& tier_config,
    const std::string& tier_name,
    const std::filesystem::path& data_dir);

/**
 * @brief Assemble the full system prompt from config.
 *
 * Loads constitution, app_context, and default tier identity, then
 * concatenates them. Used by the facade during configure.
 *
 * @param config Parsed engine config.
 * @param data_dir Bundled data directory.
 * @return Assembled system prompt string (may be empty if all disabled).
 * @version 2.0.1
 */
ENTROPIC_EXPORT std::string assemble(
    const entropic::ParsedConfig& config,
    const std::filesystem::path& data_dir);

} // namespace entropic::prompts
