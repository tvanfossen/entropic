// SPDX-License-Identifier: Apache-2.0
/**
 * @file manager.cpp
 * @brief Prompt manager implementation — frontmatter parsing, identity loading.
 * @version 1.8.2
 */

#include <entropic/prompts/manager.h>
#include <entropic/types/logging.h>
#include "yaml_util.h"

#include <ryml.hpp>
#include <c4/std/string.hpp>

static auto s_log = entropic::log::get("prompts");

// Pull yaml_util helpers into this TU to avoid qualifying every call.
using entropic::config::extract;
using entropic::config::extract_string_list;
using entropic::config::extract_string_list_opt;
using entropic::config::read_file;
using entropic::config::to_string;

namespace entropic::prompts {

/**
 * @brief Trim leading and trailing whitespace from a string.
 * @param s Input string.
 * @return Trimmed string.
 * @version 1.8.2
 * @utility
 */
static std::string trim(const std::string& s)
{
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

/**
 * @brief Parse YAML frontmatter from file content into a ryml tree.
 *
 * Splits content on "---" delimiters, validates structure, and returns
 * the parsed YAML tree plus the markdown body. Shared between
 * parse_prompt_file() and load_identity() to avoid double file reads.
 *
 * @param content Full file content.
 * @param path File path (for error messages).
 * @param[out] tree Output ryml tree of frontmatter.
 * @param[out] body Output markdown body after frontmatter.
 * @return Empty string on success, error on failure.
 * @version 1.8.2
 * @dg_internal
 */
static std::string parse_frontmatter(
    const std::string& content,
    const std::filesystem::path& path,
    ryml::Tree& tree,
    std::string& body)
{
    if (content.substr(0, 3) != "---") {
        return "prompt file " + path.string()
               + " missing YAML frontmatter";
    }

    auto second_delim = content.find("---", 3);
    if (second_delim == std::string::npos) {
        return "prompt file " + path.string()
               + " has malformed frontmatter";
    }

    std::string yaml_block = content.substr(3, second_delim - 3);
    body = trim(content.substr(second_delim + 3));

    tree = ryml::parse_in_arena(
        ryml::to_csubstr(path.string()),
        ryml::to_csubstr(yaml_block));

    return "";
}

/**
 * @brief Convert PromptType to string.
 * @param type Prompt type.
 * @return String representation.
 * @version 1.8.2
 * @utility
 */
const char* prompt_type_to_string(PromptType type)
{
    static constexpr const char* names[] = {"constitution", "app_context", "identity"};
    int idx = static_cast<int>(type);
    return (idx >= 0 && idx <= 2) ? names[idx] : "unknown";
}

/**
 * @brief Map a frontmatter type string to a PromptType.
 * @param type_str Frontmatter "type" value.
 * @param path Prompt path (for the error message).
 * @param[out] err Set to a diagnostic on an unknown type.
 * @return The PromptType (default-constructed on unknown).
 * @utility
 * @version 2.3.7
 */
static PromptType prompt_type_from_string(
    const std::string& type_str, const std::filesystem::path& path,
    std::string& err) {
    PromptType t{};
    if (type_str == "constitution") {
        t = PromptType::CONSTITUTION;
    } else if (type_str == "app_context") {
        t = PromptType::APP_CONTEXT;
    } else if (type_str == "identity") {
        t = PromptType::IDENTITY;
    } else {
        err = "prompt file " + path.string()
              + " has unknown type '" + type_str + "'";
    }
    return t;
}

/**
 * @brief Parse a prompt file: validate frontmatter, return body.
 * @param path Path to .md prompt file.
 * @param expected_type Expected frontmatter type.
 * @param[out] result Output: type, version, body.
 * @return Empty string on success, error on failure.
 * @version 2.3.7
 * @utility
 */
std::string parse_prompt_file(
    const std::filesystem::path& path,
    PromptType expected_type,
    ParsedPrompt& result)
{
    std::string err;

    auto content = read_file(path);
    if (content.empty()) {
        err = "cannot read prompt file: " + path.string();
    }

    ryml::Tree tree;
    if (err.empty()) {
        err = parse_frontmatter(content, path, tree, result.body);
    }

    ryml::ConstNodeRef root;
    std::string type_str;
    if (err.empty()) {
        root = tree.rootref();
        if (!extract(root, "type", type_str)) {
            err = "prompt file " + path.string()
                  + " missing 'type' field";
        }
    }

    PromptType actual_type{};
    if (err.empty()) {
        actual_type = prompt_type_from_string(type_str, path, err);
    }

    if (err.empty() && actual_type != expected_type) {
        err = "prompt file " + path.string() + " has type '"
              + type_str + "' but was loaded as '"
              + prompt_type_to_string(expected_type) + "'";
    }

    if (err.empty()) {
        result.type = actual_type;
        extract(root, "version", result.version);
    }

    return err;
}

/**
 * @brief Extract phase configs from identity frontmatter.
 * @param root ryml root node.
 * @param[out] fm Output identity frontmatter.
 * @version 1.8.2
 * @dg_internal
 */
static void extract_phases(
    ryml::ConstNodeRef root, IdentityFrontmatter& fm)
{
    if (!root.has_child("phases") || !root["phases"].is_map()) {
        return;
    }
    fm.phases.emplace();
    for (auto child : root["phases"]) {
        PhaseConfig phase;
        std::string phase_name = to_string(child.key());
        extract(child, "temperature", phase.temperature);
        extract(child, "max_output_tokens", phase.max_output_tokens);
        extract(child, "enable_thinking", phase.enable_thinking);
        extract(child, "repeat_penalty", phase.repeat_penalty);
        extract_string_list_opt(child, "bash_commands", phase.bash_commands);
        (*fm.phases)[phase_name] = std::move(phase);
    }
}

/**
 * @brief Extract benchmark config from identity frontmatter.
 * @param root ryml root node.
 * @param[out] fm Output identity frontmatter.
 * @version 1.8.2
 * @dg_internal
 */
static void extract_benchmark(
    ryml::ConstNodeRef root, IdentityFrontmatter& fm)
{
    if (!root.has_child("benchmark") || !root["benchmark"].is_map()) {
        return;
    }
    fm.benchmark.emplace();
    auto bench = root["benchmark"];
    if (!bench.has_child("prompts") || !bench["prompts"].is_seq()) {
        return;
    }
    for (auto p_node : bench["prompts"]) {
        BenchmarkPrompt bp;
        extract(p_node, "prompt", bp.prompt);
        if (p_node.has_child("checks") && p_node["checks"].is_seq()) {
            for (auto c_node : p_node["checks"]) {
                std::string check_yaml;
                ryml::emitrs_yaml(c_node, &check_yaml);
                bp.checks_yaml.push_back(std::move(check_yaml));
            }
        }
        fm.benchmark->prompts.push_back(std::move(bp));
    }
}

/**
 * @brief Extract identity-specific fields from a pre-parsed ryml tree.
 * @param root ryml root node of the frontmatter.
 * @param[out] fm Output identity frontmatter.
 * @version 2.0.6-rc18
 * @dg_internal
 */
/**
 * @brief Extract the per-tier sampler knobs (gh#82/gh#85/gh#86).
 *
 * Each is optional — set only when the key is present so the
 * orchestrator can distinguish a configured per-tier value from "not
 * set" (and leave the GenerationParams default in place). Split from
 * extract_identity_flags to keep both under the knots ABC gate.
 *
 * @param root YAML root node.
 * @param[out] fm Identity frontmatter to populate.
 * @utility
 * @version 2.5.4
 */
static void extract_sampler_knobs(ryml::ConstNodeRef root,
                                  IdentityFrontmatter& fm) {
    int ival = 0;
    if (extract(root, "max_output_tokens", ival)) { fm.max_output_tokens = ival; }
    if (extract(root, "top_k", ival)) { fm.top_k = ival; }
    float fval = 0.0f;
    if (extract(root, "temperature", fval)) { fm.temperature = fval; }
    if (extract(root, "top_p", fval)) { fm.top_p = fval; }
    if (extract(root, "min_p", fval)) { fm.min_p = fval; }
    if (extract(root, "presence_penalty", fval)) { fm.presence_penalty = fval; }
    if (extract(root, "frequency_penalty", fval)) { fm.frequency_penalty = fval; }
    if (extract(root, "repeat_penalty", fval)) { fm.repeat_penalty = fval; }
    // enable_thinking's GenerationParams default is true, so "not set"
    // must stay distinct from an explicit false (gh#86).
    bool think = false;
    if (extract(root, "enable_thinking", think)) { fm.enable_thinking = think; }
}

/**
 * @brief Extract the scalar loop/flag fields of an identity.
 * @param root YAML root node.
 * @param[out] fm Identity frontmatter to populate.
 * @utility
 * @version 2.9.20
 */
static void extract_identity_flags(ryml::ConstNodeRef root,
                                   IdentityFrontmatter& fm) {
    extract_sampler_knobs(root, fm);  // gh#82/gh#85/gh#86
    extract(root, "interstitial", fm.interstitial);
    extract(root, "routable", fm.routable);
    bool ec_val{};
    if (extract(root, "explicit_completion", ec_val)) {
        fm.explicit_completion = ec_val;
    }
    extract(root, "relay_single_delegate", fm.relay_single_delegate);
    // E6 (2.0.6-rc18): per-identity loop + tool-call caps
    extract(root, "max_iterations", fm.max_iterations);
    extract(root, "max_tool_calls_per_turn", fm.max_tool_calls_per_turn);
    extract(root, "max_consecutive_empty_turns", fm.max_consecutive_empty_turns);
    extract_string_list(root, "validation_rules", fm.validation_rules);
}

/**
 * @brief Extract all identity frontmatter fields into the struct.
 * @dg_internal
 * @version 2.3.7
 */
static void extract_identity_fields(
    ryml::ConstNodeRef root, IdentityFrontmatter& fm)
{
    fm.type = PromptType::IDENTITY;
    extract(root, "version", fm.version);
    extract(root, "name", fm.name);
    extract_string_list(root, "focus", fm.focus);
    extract_string_list(root, "examples", fm.examples);

    std::string grammar_str;
    if (extract(root, "grammar", grammar_str)) {
        fm.grammar = grammar_str;
    }
    std::string auto_chain_str;
    if (extract(root, "auto_chain", auto_chain_str)) {
        fm.auto_chain = auto_chain_str;
    }

    extract_string_list_opt(root, "allowed_tools", fm.allowed_tools);
    extract_string_list_opt(root, "bash_commands", fm.bash_commands);

    extract_identity_flags(root, fm);

    extract_phases(root, fm);
    extract_benchmark(root, fm);
}

/**
 * @brief Load an identity file: parse frontmatter + body.
 * @param path Path to identity .md file.
 * @param[out] identity Output parsed identity.
 * @return Empty string on success, error on failure.
 * @version 1.8.2
 * @utility
 */
std::string load_identity(
    const std::filesystem::path& path,
    ParsedIdentity& identity)
{
    std::string err;

    auto content = read_file(path);
    if (content.empty()) {
        err = "cannot read identity file: " + path.string();
    }

    ryml::Tree tree;
    if (err.empty()) {
        err = parse_frontmatter(content, path, tree, identity.body);
    }

    ryml::ConstNodeRef root;
    if (err.empty()) {
        root = tree.rootref();

        // Validate type field
        std::string type_str;
        if (!extract(root, "type", type_str) || type_str != "identity") {
            err = "prompt file " + path.string()
                  + " is not an identity file (type='"
                  + type_str + "')";
        }
    }

    if (err.empty()) {
        extract_identity_fields(root, identity.frontmatter);

        if (identity.frontmatter.focus.empty()) {
            err = "identity " + path.string()
                  + ": focus must have at least one entry";
        } else if (identity.frontmatter.name.empty()) {
            err = "identity " + path.string()
                  + ": name must not be empty";
        }
    }

    return err;
}

/**
 * @brief Load constitution prompt with tri-state resolution.
 * @param constitution_path Custom path (nullopt = bundled).
 * @param disabled true if constitution explicitly disabled.
 * @param data_dir Bundled data directory.
 * @param[out] body Output constitution text.
 * @return Empty string on success, error on failure.
 * @version 1.8.2
 * @utility
 */
std::string load_constitution(
    const std::optional<std::filesystem::path>& constitution_path,
    bool disabled,
    const std::filesystem::path& data_dir,
    std::string& body)
{
    std::string err;

    if (disabled) {
        s_log->info("Constitution disabled by config");
        body.clear();
    } else {
        std::filesystem::path path = constitution_path.has_value()
            ? *constitution_path
            : data_dir / "prompts" / "constitution.md";

        if (!std::filesystem::exists(path)) {
            err = "constitution file not found: " + path.string();
        }

        ParsedPrompt result;
        if (err.empty()) {
            err = parse_prompt_file(path, PromptType::CONSTITUTION, result);
        }

        if (err.empty()) {
            body = std::move(result.body);
            s_log->info("Constitution loaded from {}", path.string());
        }
    }

    return err;
}

/**
 * @brief Read app_context from a file, resolving bare names as bundled.
 *
 * Split out of load_app_context in v2.11.0: adding the inline-content branch
 * (gh#141) pushed that function to the top of the complexity report, and the
 * two sources are independent concerns.
 *
 * @param app_context_path Configured path; a bare filename resolves under
 *        data_dir/prompts/, anything with a directory component is used as-is.
 * @param data_dir Bundled data directory.
 * @param[out] body Receives the parsed prompt body.
 * @return Empty string on success, an error message otherwise.
 * @dg_internal
 * @version 2.11.0
 */
static std::string load_app_context_file(
    const std::filesystem::path& app_context_path,
    const std::filesystem::path& data_dir,
    std::string& body)
{
    auto path = app_context_path;

    // Bare filename resolves as bundled prompt
    if (!path.has_parent_path() || path.parent_path().empty()) {
        path = data_dir / "prompts" / path;
    }

    std::string err;
    if (!std::filesystem::exists(path)) {
        err = "app_context file not found: " + path.string();
    }

    ParsedPrompt result;
    if (err.empty()) {
        err = parse_prompt_file(path, PromptType::APP_CONTEXT, result);
    }

    if (err.empty()) {
        body = std::move(result.body);
        s_log->info("App context loaded from {}", path.string());
    }

    return err;
}

/**
 * @brief Load app_context from inline content or a path, with tri-state resolution.
 *
 * Resolution order (gh#141): an explicit opt-out wins over everything; then
 * inline content, which is used without touching the filesystem; then a path.
 *
 * @param app_context_path Custom path (nullopt = none configured).
 * @param app_context_content Inline text (nullopt = none supplied).
 * @param disabled true if app_context explicitly disabled.
 * @param data_dir Bundled data directory.
 * @param[out] body Output app_context text.
 * @return Empty string on success, error on failure.
 * @req REQ-TYPE-005
 * @version 2.11.0
 * @utility
 */
std::string load_app_context(
    const std::optional<std::filesystem::path>& app_context_path,
    const std::optional<std::string>& app_context_content,
    bool disabled,
    const std::filesystem::path& data_dir,
    std::string& body)
{
    std::string err;

    // gh#141: inline content wins over a path, but NOT over an explicit
    // opt-out — `app_context: false` still means off, whatever else is set.
    // Checked before the path branch so the filesystem is never touched when
    // the caller already holds the text; that is the whole point of the
    // feature for a consumer that cannot write the file.
    if (!disabled && app_context_content.has_value()) {
        body = *app_context_content;
        s_log->info("App context supplied inline ({} bytes)", body.size());
        return err;
    }

    if (disabled || !app_context_path.has_value()) {
        s_log->info("App context disabled (not configured)");
        body.clear();
    } else {
        err = load_app_context_file(*app_context_path, data_dir, body);
    }

    return err;
}

/**
 * @brief Load both shared prompt sources, logging rather than discarding.
 * @param config Parsed engine config.
 * @param data_dir Bundled data directory.
 * @param[out] constitution Constitution body (empty when it failed).
 * @param[out] app_context App context body (empty when it failed).
 * @req REQ-TYPE-005
 * @version 2.13.0
 */
void load_shared_prompt_sources(
    const entropic::ParsedConfig& config,
    const std::filesystem::path& data_dir,
    std::string& constitution,
    std::string& app_context)
{
    // gh#156: these two return values were dropped at all three call
    // sites. A working load announced itself ("App context loaded
    // from ..."), a failing one said nothing — the asymmetry WAS the
    // bug, and it cost a consumer twenty minutes.
    auto con_err = load_constitution(
        config.constitution, config.constitution_disabled,
        data_dir, constitution);
    if (!con_err.empty()) {
        s_log->error("constitution ignored: {}", con_err);
    }
    auto ctx_err = load_app_context(
        config.app_context, config.app_context_content,
        config.app_context_disabled, data_dir, app_context);
    if (!ctx_err.empty()) {
        s_log->error("app_context ignored: {}", ctx_err);
    }
}

/**
 * @brief Check a configured constitution path loads.
 * @param config Parsed engine config.
 * @param data_dir Bundled data directory.
 * @return Empty string when unconfigured, disabled, or loadable.
 * @dg_internal
 * @version 2.13.0
 */
static std::string check_constitution_source(
    const entropic::ParsedConfig& config,
    const std::filesystem::path& data_dir)
{
    std::string err;
    if (config.constitution.has_value() && !config.constitution_disabled) {
        std::string body;
        err = load_constitution(config.constitution, false, data_dir, body);
        if (!err.empty()) {
            err = "constitution: " + err
                + ". A constitution file is one of entropic's own prompt "
                  "documents: it needs YAML frontmatter with "
                  "`type: constitution` between `---` lines.";
        }
    }
    return err;
}

/**
 * @brief Check a configured app_context path loads.
 *
 * Inline content (gh#141) and `app_context: false` are never checked —
 * neither touches the filesystem, and both must keep working untouched.
 *
 * @param config Parsed engine config.
 * @param data_dir Bundled data directory.
 * @return Empty string when unconfigured, disabled, inline, or loadable.
 * @dg_internal
 * @version 2.13.0
 */
static std::string check_app_context_source(
    const entropic::ParsedConfig& config,
    const std::filesystem::path& data_dir)
{
    const bool configured_as_path = config.app_context.has_value()
        && !config.app_context_disabled
        && !config.app_context_content.has_value();
    std::string err;
    if (configured_as_path) {
        std::string body;
        err = load_app_context(config.app_context, std::nullopt, false,
                               data_dir, body);
        if (!err.empty()) {
            err = "app_context: " + err
                + ". app_context is read as one of entropic's own prompt "
                  "documents, so the file needs YAML frontmatter with "
                  "`type: app_context` between `---` lines. To supply a "
                  "document you do not own — a repository's CLAUDE.md, for "
                  "example — pass the text inline instead of a path: "
                  "`app_context: {content: \"...\"}` (gh#141).";
        }
    }
    return err;
}

/**
 * @brief Check every configured prompt source can be loaded.
 * @param config Parsed engine config.
 * @param data_dir Bundled data directory.
 * @return Empty string on success, an actionable message otherwise.
 * @req REQ-TYPE-005
 * @version 2.13.0
 */
std::string validate_configured_prompts(
    const entropic::ParsedConfig& config,
    const std::filesystem::path& data_dir)
{
    std::string err = check_constitution_source(config, data_dir);
    if (err.empty()) {
        err = check_app_context_source(config, data_dir);
    }
    return err;
}

/**
 * @brief Resolve a full parsed identity (body + frontmatter) for a tier.
 *
 * Path convention:
 * 1. If tier has explicit identity path → use it
 * 2. If identity not disabled → data_dir/prompts/identity_{tier_name}.md
 * 3. If disabled or not found → empty ParsedIdentity
 *
 * @param tier_config Tier configuration.
 * @param tier_name Tier name (for default path convention).
 * @param data_dir Bundled data directory.
 * @return ParsedIdentity; body empty when no identity resolved.
 * @utility
 * @version 2.0.6-rc18
 */
ParsedIdentity resolve_tier_identity_full(
    const entropic::TierConfig& tier_config,
    const std::string& tier_name,
    const std::filesystem::path& data_dir)
{
    std::filesystem::path id_path;
    if (tier_config.identity.has_value()) {
        id_path = tier_config.identity.value();
    } else if (!tier_config.identity_disabled) {
        id_path = data_dir / "prompts"
            / ("identity_" + tier_name + ".md");
    }
    ParsedIdentity id;
    if (id_path.empty() || !std::filesystem::exists(id_path)) {
        return id;
    }
    auto err = load_identity(id_path, id);
    if (!err.empty()) {
        s_log->warn("identity load failed for tier '{}': {}",
                     tier_name, err);
        return ParsedIdentity{};
    }
    s_log->info("identity loaded for tier '{}' from {}",
                 tier_name, id_path.string());
    return id;
}

/**
 * @brief Body-only wrapper around resolve_tier_identity_full.
 * @param tier_config Tier configuration.
 * @param tier_name Tier name (for default path convention).
 * @param data_dir Bundled data directory.
 * @return Identity body string (empty if disabled or not found).
 * @utility
 * @version 2.0.6-rc18
 */
std::string resolve_tier_identity(
    const entropic::TierConfig& tier_config,
    const std::string& tier_name,
    const std::filesystem::path& data_dir)
{
    return resolve_tier_identity_full(
        tier_config, tier_name, data_dir).body;
}

/**
 * @brief Assemble the full system prompt from config.
 *
 * Loads constitution, app_context, and default tier identity,
 * then concatenates. The data_dir is used for bundled fallback paths.
 *
 * @param config Parsed engine config.
 * @param data_dir Bundled data directory.
 * @return Assembled system prompt string.
 * @dg_internal
 * @version 2.13.0
 */
std::string assemble(
    const entropic::ParsedConfig& config,
    const std::filesystem::path& data_dir) {
    std::string constitution, app_ctx;

    load_shared_prompt_sources(config, data_dir, constitution, app_ctx);

    std::string identity_body;
    auto tier_it = config.models.tiers.find(config.models.default_tier);
    if (tier_it != config.models.tiers.end()) {
        identity_body = resolve_tier_identity(
            tier_it->second, config.models.default_tier, data_dir);
    }

    std::string prompt;
    if (!constitution.empty()) { prompt += constitution + "\n\n"; }
    if (!app_ctx.empty()) { prompt += app_ctx + "\n\n"; }
    if (!identity_body.empty()) { prompt += identity_body; }

    s_log->info("system prompt assembled: {} chars "
                "(constitution={}, app_context={}, identity={})",
                prompt.size(), !constitution.empty(),
                !app_ctx.empty(), !identity_body.empty());
    return prompt;
}

} // namespace entropic::prompts
