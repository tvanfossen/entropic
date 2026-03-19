/**
 * @file loader.cpp
 * @brief Config loader implementation — YAML parsing + layered merge.
 * @version 1.8.2
 */

#include <entropic/config/loader.h>
#include <entropic/config/validate.h>
#include <entropic/types/logging.h>
#include "yaml_util.h"

static auto s_log = entropic::log::get("config");

namespace entropic::config {

/**
 * @brief Parse a ModelConfig from a YAML node.
 * @param node YAML node containing model fields.
 * @param registry Bundled models for path resolution.
 * @param[out] config Output model config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_model_config(
    ryml::ConstNodeRef node,
    const BundledModels& registry,
    ModelConfig& config)
{
    std::string path_str;
    if (extract(node, "path", path_str)) {
        config.path = registry.resolve(path_str);
    }

    extract(node, "adapter", config.adapter);
    extract(node, "context_length", config.context_length);
    extract(node, "gpu_layers", config.gpu_layers);
    extract(node, "keep_warm", config.keep_warm);
    extract(node, "use_mlock", config.use_mlock);
    extract(node, "reasoning_budget", config.reasoning_budget);
    extract(node, "cache_type_k", config.cache_type_k);
    extract(node, "cache_type_v", config.cache_type_v);
    extract(node, "n_batch", config.n_batch);
    extract(node, "n_threads", config.n_threads);
    extract(node, "tensor_split", config.tensor_split);
    extract(node, "flash_attn", config.flash_attn);
    extract_string_list_opt(node, "allowed_tools", config.allowed_tools);

    return "";
}

/**
 * @brief Parse a TierConfig from a YAML node.
 * @param node YAML node containing tier fields.
 * @param registry Bundled models for path resolution.
 * @param[out] config Output tier config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_tier_config(
    ryml::ConstNodeRef node,
    const BundledModels& registry,
    TierConfig& config)
{
    auto err = parse_model_config(node, registry, config);
    if (!err.empty()) {
        return err;
    }

    extract_tri_state_path(node, "identity",
                           config.identity, config.identity_disabled);

    std::string grammar_str;
    if (extract(node, "grammar", grammar_str)) {
        config.grammar = expand_home(std::filesystem::path(grammar_str));
    }

    std::string auto_chain_str;
    if (extract(node, "auto_chain", auto_chain_str)) {
        config.auto_chain = auto_chain_str;
    }

    bool routable_val = false;
    if (extract(node, "routable", routable_val)) {
        config.routable = routable_val;
    }

    return "";
}

/**
 * @brief Parse the models section from a YAML node.
 * @param node YAML node for "models" section.
 * @param registry Bundled models for path resolution.
 * @param[out] config Output models config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_models_config(
    ryml::ConstNodeRef node,
    const BundledModels& registry,
    ModelsConfig& config)
{
    extract(node, "default", config.default_tier);

    if (node.has_child("router")) {
        config.router.emplace();
        auto err = parse_model_config(node["router"], registry,
                                      *config.router);
        if (!err.empty()) {
            return "models.router: " + err;
        }
    }

    for (auto child : node) {
        std::string key = to_string(child.key());
        if (key == "default" || key == "router") {
            continue;
        }
        if (!child.is_map()) {
            continue;
        }

        TierConfig tier;
        if (config.tiers.count(key) > 0) {
            tier = config.tiers[key];
        }
        auto err = parse_tier_config(child, registry, tier);
        if (!err.empty()) {
            return "models." + key + ": " + err;
        }
        config.tiers[key] = std::move(tier);
    }

    return "";
}

/**
 * @brief Parse the routing section from a YAML node.
 * @param node YAML node for "routing" section.
 * @param[out] config Output routing config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_routing_config(
    ryml::ConstNodeRef node,
    RoutingConfig& config)
{
    extract(node, "enabled", config.enabled);
    extract(node, "fallback_tier", config.fallback_tier);

    std::string class_prompt;
    if (extract(node, "classification_prompt", class_prompt)) {
        config.classification_prompt = class_prompt;
    }

    extract_string_map(node, "tier_map", config.tier_map);
    extract_string_list_map(node, "handoff_rules", config.handoff_rules);

    return "";
}

/**
 * @brief Parse the compaction section from a YAML node.
 * @param node YAML node for "compaction" section.
 * @param[out] config Output compaction config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_compaction_config(
    ryml::ConstNodeRef node,
    CompactionConfig& config)
{
    extract(node, "enabled", config.enabled);
    extract(node, "threshold_percent", config.threshold_percent);
    extract(node, "preserve_recent_turns", config.preserve_recent_turns);
    extract(node, "summary_max_tokens", config.summary_max_tokens);
    extract(node, "notify_user", config.notify_user);
    extract(node, "save_full_history", config.save_full_history);
    extract(node, "tool_result_ttl", config.tool_result_ttl);
    extract(node, "warning_threshold_percent",
            config.warning_threshold_percent);
    return "";
}

/**
 * @brief Parse the permissions section from a YAML node.
 * @param node YAML node for "permissions" section.
 * @param[out] config Output permissions config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_permissions_config(
    ryml::ConstNodeRef node,
    PermissionsConfig& config)
{
    extract_string_list(node, "allow", config.allow);
    extract_string_list(node, "deny", config.deny);
    extract(node, "auto_approve", config.auto_approve);
    return "";
}

/**
 * @brief Parse the filesystem section from a YAML node.
 * @param node YAML node for "filesystem" section.
 * @param[out] config Output filesystem config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_filesystem_config(
    ryml::ConstNodeRef node,
    FilesystemConfig& config)
{
    extract(node, "diagnostics_on_edit", config.diagnostics_on_edit);
    extract(node, "fail_on_errors", config.fail_on_errors);
    extract(node, "diagnostics_timeout", config.diagnostics_timeout);
    extract(node, "allow_outside_root", config.allow_outside_root);
    extract(node, "max_read_context_pct", config.max_read_context_pct);

    int max_read = 0;
    if (extract(node, "max_read_bytes", max_read)) {
        config.max_read_bytes = max_read;
    }

    return "";
}

/**
 * @brief Parse the external MCP section from a YAML node.
 * @param node YAML node for "external" section.
 * @param[out] config Output external MCP config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_external_mcp_config(
    ryml::ConstNodeRef node,
    ExternalMCPConfig& config)
{
    extract(node, "enabled", config.enabled);
    extract(node, "rate_limit", config.rate_limit);

    if (node.is_map() && node.has_child("socket_path")
        && !node["socket_path"].val_is_null()) {
        std::filesystem::path tmp;
        extract_path(node, "socket_path", tmp);
        config.socket_path = tmp;
    }

    return "";
}

/**
 * @brief Parse the MCP section from a YAML node.
 * @param node YAML node for "mcp" section.
 * @param[out] config Output MCP config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_mcp_config(
    ryml::ConstNodeRef node,
    MCPConfig& config)
{
    extract(node, "enable_filesystem", config.enable_filesystem);
    extract(node, "enable_bash", config.enable_bash);
    extract(node, "enable_git", config.enable_git);
    extract(node, "enable_diagnostics", config.enable_diagnostics);
    extract(node, "enable_web", config.enable_web);
    extract(node, "server_timeout_seconds", config.server_timeout_seconds);

    if (node.has_child("filesystem")) {
        parse_filesystem_config(node["filesystem"], config.filesystem);
    }
    if (node.has_child("external")) {
        parse_external_mcp_config(node["external"], config.external);
    }

    return "";
}

/**
 * @brief Parse the generation section from a YAML node.
 * @param node YAML node for "generation" section.
 * @param[out] config Output generation config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_generation_config(
    ryml::ConstNodeRef node,
    GenerationConfig& config)
{
    extract(node, "max_tokens", config.max_tokens);
    extract(node, "default_temperature", config.default_temperature);
    extract(node, "default_top_p", config.default_top_p);
    return "";
}

/**
 * @brief Parse the LSP section from a YAML node.
 * @param node YAML node for "lsp" section.
 * @param[out] config Output LSP config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
static std::string parse_lsp_config(
    ryml::ConstNodeRef node,
    LSPConfig& config)
{
    extract(node, "enabled", config.enabled);
    extract(node, "python_enabled", config.python_enabled);
    extract(node, "c_enabled", config.c_enabled);
    return "";
}

/**
 * @brief Parse optional config sections that don't return errors.
 * @param root YAML root node.
 * @param config Config to populate.
 * @internal
 * @version 1.8.2
 */
static void parse_optional_sections(
    ryml::ConstNodeRef root, ParsedConfig& config)
{
    if (root.has_child("generation"))
        parse_generation_config(root["generation"], config.generation);
    if (root.has_child("permissions"))
        parse_permissions_config(root["permissions"], config.permissions);
    if (root.has_child("mcp"))
        parse_mcp_config(root["mcp"], config.mcp);
    if (root.has_child("compaction"))
        parse_compaction_config(root["compaction"], config.compaction);
    if (root.has_child("lsp"))
        parse_lsp_config(root["lsp"], config.lsp);

    extract(root, "log_level", config.log_level);
    extract(root, "inject_model_context", config.inject_model_context);
    extract(root, "vram_reserve_mb", config.vram_reserve_mb);
    extract_path(root, "config_dir", config.config_dir);

    extract_tri_state_path(root, "constitution",
                           config.constitution, config.constitution_disabled);
    extract_tri_state_path(root, "app_context",
                           config.app_context, config.app_context_disabled);
}

/**
 * @brief Parse a config YAML file and overlay onto existing config.
 * @param path Path to YAML file.
 * @param registry Bundled models for path resolution.
 * @param[in,out] config Config to overlay onto.
 * @return Empty string on success, error message on failure.
 * @req REQ-CFG-001
 * @version 1.8.2
 */
std::string parse_config_file(
    const std::filesystem::path& path,
    const BundledModels& registry,
    ParsedConfig& config)
{
    std::string err;

    auto content = read_file(path);
    if (content.empty()) {
        err = "cannot read config file: " + path.string();
    }

    ryml::Tree tree;
    ryml::ConstNodeRef root;
    if (err.empty()) {
        tree = ryml::parse_in_arena(
            ryml::to_csubstr(path.string()),
            ryml::to_csubstr(content));
        root = tree.rootref();
        if (!root.is_map()) {
            err = "config file root is not a YAML mapping: " + path.string();
        }
    }

    if (err.empty() && root.has_child("models")) {
        err = parse_models_config(root["models"], registry, config.models);
    }
    if (err.empty() && root.has_child("routing")) {
        err = parse_routing_config(root["routing"], config.routing);
    }
    if (err.empty()) {
        parse_optional_sections(root, config);
    }

    return err;
}

/**
 * @brief Load config using layered resolution.
 * @param global_path Path to global config file.
 * @param project_path Path to project config file.
 * @param registry Bundled models registry.
 * @param[out] config Output parsed config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
std::string load_config(
    const std::filesystem::path& global_path,
    const std::filesystem::path& project_path,
    const BundledModels& registry,
    ParsedConfig& config)
{
    if (std::filesystem::exists(global_path)) {
        s_log->info("Loading global config: {}", global_path.string());
        auto err = parse_config_file(global_path, registry, config);
        if (!err.empty()) {
            return "global config: " + err;
        }
    }

    if (std::filesystem::exists(project_path)) {
        s_log->info("Loading project config: {}", project_path.string());
        auto err = parse_config_file(project_path, registry, config);
        if (!err.empty()) {
            return "project config: " + err;
        }
    }

    apply_env_overrides(config);

    std::vector<std::string> warnings;
    auto err = validate_config(config, warnings);
    for (const auto& w : warnings) {
        s_log->warn("{}", w);
    }
    return err;
}

/**
 * @brief Load config from a single YAML file.
 * @param path Path to YAML config file.
 * @param registry Bundled models registry.
 * @param[out] config Output parsed config.
 * @return Empty string on success, error message on failure.
 * @version 1.8.2
 */
std::string load_config_from_file(
    const std::filesystem::path& path,
    const BundledModels& registry,
    ParsedConfig& config)
{
    auto err = parse_config_file(path, registry, config);
    if (!err.empty()) {
        return err;
    }

    apply_env_overrides(config);

    std::vector<std::string> warnings;
    err = validate_config(config, warnings);
    for (const auto& w : warnings) {
        s_log->warn("{}", w);
    }
    return err;
}

} // namespace entropic::config
