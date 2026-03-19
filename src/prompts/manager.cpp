/**
 * @file manager.cpp
 * @brief Prompt manager implementation — frontmatter parsing, identity loading.
 * @version 1.8.1
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
 * @version 1.8.1
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
 * @version 1.8.1
 * @internal
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
 * @version 1.8.1
 * @utility
 */
const char* prompt_type_to_string(PromptType type)
{
    switch (type) {
    case PromptType::CONSTITUTION:
        return "constitution";
    case PromptType::APP_CONTEXT:
        return "app_context";
    case PromptType::IDENTITY:
        return "identity";
    }
    return "unknown";
}

/**
 * @brief Parse a prompt file: validate frontmatter, return body.
 * @param path Path to .md prompt file.
 * @param expected_type Expected frontmatter type.
 * @param[out] result Output: type, version, body.
 * @return Empty string on success, error on failure.
 * @version 1.8.1
 * @utility
 */
std::string parse_prompt_file(
    const std::filesystem::path& path,
    PromptType expected_type,
    ParsedPrompt& result)
{
    auto content = read_file(path);
    if (content.empty()) {
        return "cannot read prompt file: " + path.string();
    }

    ryml::Tree tree;
    auto err = parse_frontmatter(content, path, tree, result.body);
    if (!err.empty()) {
        return err;
    }

    ryml::ConstNodeRef root = tree.rootref();

    std::string type_str;
    if (!extract(root, "type", type_str)) {
        return "prompt file " + path.string()
               + " missing 'type' field";
    }

    PromptType actual_type;
    if (type_str == "constitution") {
        actual_type = PromptType::CONSTITUTION;
    } else if (type_str == "app_context") {
        actual_type = PromptType::APP_CONTEXT;
    } else if (type_str == "identity") {
        actual_type = PromptType::IDENTITY;
    } else {
        return "prompt file " + path.string()
               + " has unknown type '" + type_str + "'";
    }

    if (actual_type != expected_type) {
        return "prompt file " + path.string() + " has type '"
               + type_str + "' but was loaded as '"
               + prompt_type_to_string(expected_type) + "'";
    }

    result.type = actual_type;
    extract(root, "version", result.version);

    return "";
}

/**
 * @brief Extract identity-specific fields from a pre-parsed ryml tree.
 * @param root ryml root node of the frontmatter.
 * @param[out] fm Output identity frontmatter.
 * @version 2
 * @internal
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

    extract(root, "max_output_tokens", fm.max_output_tokens);
    extract(root, "temperature", fm.temperature);
    extract(root, "repeat_penalty", fm.repeat_penalty);
    extract(root, "enable_thinking", fm.enable_thinking);
    extract(root, "model_preference", fm.model_preference);
    extract(root, "interstitial", fm.interstitial);
    extract(root, "routable", fm.routable);
    extract(root, "role_type", fm.role_type);
    extract(root, "explicit_completion", fm.explicit_completion);

    // Phases
    if (root.has_child("phases") && root["phases"].is_map()) {
        fm.phases.emplace();
        for (auto child : root["phases"]) {
            PhaseConfig phase;
            std::string phase_name = to_string(child.key());
            extract(child, "temperature", phase.temperature);
            extract(child, "max_output_tokens", phase.max_output_tokens);
            extract(child, "enable_thinking", phase.enable_thinking);
            extract(child, "repeat_penalty", phase.repeat_penalty);
            extract_string_list_opt(
                child, "bash_commands", phase.bash_commands);
            (*fm.phases)[phase_name] = std::move(phase);
        }
    }

    // Benchmark
    if (root.has_child("benchmark") && root["benchmark"].is_map()) {
        fm.benchmark.emplace();
        auto bench_node = root["benchmark"];
        if (bench_node.has_child("prompts")
            && bench_node["prompts"].is_seq()) {
            for (auto p_node : bench_node["prompts"]) {
                BenchmarkPrompt bp;
                extract(p_node, "prompt", bp.prompt);
                if (p_node.has_child("checks")
                    && p_node["checks"].is_seq()) {
                    for (auto c_node : p_node["checks"]) {
                        std::string check_yaml;
                        ryml::emitrs_yaml(c_node, &check_yaml);
                        bp.checks_yaml.push_back(
                            std::move(check_yaml));
                    }
                }
                fm.benchmark->prompts.push_back(std::move(bp));
            }
        }
    }
}

/**
 * @brief Load an identity file: parse frontmatter + body.
 * @param path Path to identity .md file.
 * @param[out] identity Output parsed identity.
 * @return Empty string on success, error on failure.
 * @version 1.8.1
 * @utility
 */
std::string load_identity(
    const std::filesystem::path& path,
    ParsedIdentity& identity)
{
    auto content = read_file(path);
    if (content.empty()) {
        return "cannot read identity file: " + path.string();
    }

    ryml::Tree tree;
    auto err = parse_frontmatter(content, path, tree, identity.body);
    if (!err.empty()) {
        return err;
    }

    ryml::ConstNodeRef root = tree.rootref();

    // Validate type field
    std::string type_str;
    if (!extract(root, "type", type_str) || type_str != "identity") {
        return "prompt file " + path.string()
               + " is not an identity file (type='"
               + type_str + "')";
    }

    extract_identity_fields(root, identity.frontmatter);

    if (identity.frontmatter.focus.empty()) {
        return "identity " + path.string()
               + ": focus must have at least one entry";
    }
    if (identity.frontmatter.name.empty()) {
        return "identity " + path.string()
               + ": name must not be empty";
    }

    return "";
}

/**
 * @brief Load constitution prompt with tri-state resolution.
 * @param constitution_path Custom path (nullopt = bundled).
 * @param disabled true if constitution explicitly disabled.
 * @param data_dir Bundled data directory.
 * @param[out] body Output constitution text.
 * @return Empty string on success, error on failure.
 * @version 1.8.1
 * @utility
 */
std::string load_constitution(
    const std::optional<std::filesystem::path>& constitution_path,
    bool disabled,
    const std::filesystem::path& data_dir,
    std::string& body)
{
    if (disabled) {
        s_log->info("Constitution disabled by config");
        body.clear();
        return "";
    }

    std::filesystem::path path;
    if (constitution_path.has_value()) {
        path = *constitution_path;
    } else {
        path = data_dir / "prompts" / "constitution.md";
    }

    if (!std::filesystem::exists(path)) {
        return "constitution file not found: " + path.string();
    }

    ParsedPrompt result;
    auto err = parse_prompt_file(path, PromptType::CONSTITUTION, result);
    if (!err.empty()) {
        return err;
    }

    body = std::move(result.body);
    s_log->info("Constitution loaded from {}", path.string());
    return "";
}

/**
 * @brief Load app_context prompt with tri-state resolution.
 * @param app_context_path Custom path (nullopt = disabled).
 * @param disabled true if app_context explicitly disabled.
 * @param data_dir Bundled data directory.
 * @param[out] body Output app_context text.
 * @return Empty string on success, error on failure.
 * @version 1.8.1
 * @utility
 */
std::string load_app_context(
    const std::optional<std::filesystem::path>& app_context_path,
    bool disabled,
    const std::filesystem::path& data_dir,
    std::string& body)
{
    if (disabled || !app_context_path.has_value()) {
        s_log->info("App context disabled (not configured)");
        body.clear();
        return "";
    }

    auto path = *app_context_path;

    // Bare filename resolves as bundled prompt
    if (!path.has_parent_path() || path.parent_path().empty()) {
        path = data_dir / "prompts" / path;
    }

    if (!std::filesystem::exists(path)) {
        return "app_context file not found: " + path.string();
    }

    ParsedPrompt result;
    auto err = parse_prompt_file(path, PromptType::APP_CONTEXT, result);
    if (!err.empty()) {
        return err;
    }

    body = std::move(result.body);
    s_log->info("App context loaded from {}", path.string());
    return "";
}

} // namespace entropic::prompts
