// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file loader.cpp
 * @brief Config loader implementation — YAML parsing + layered merge.
 * @version 1.8.2
 */

#include <entropic/config/loader.h>
#include <entropic/config/validate.h>
#include <entropic/types/logging.h>
#include "yaml_util.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

#include <dlfcn.h>

static auto s_log = entropic::log::get("config");

namespace entropic::config {

/**
 * @brief Parse a ModelConfig from a YAML node.
 * @param node YAML node containing model fields.
 * @param registry Bundled models for path resolution.
 * @param[out] config Output model config.
 * @return Empty string on success, error message on failure.
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
 * @version 2.0.4
 */
static std::string parse_mcp_config(
    ryml::ConstNodeRef node,
    MCPConfig& config)
{
    extract(node, "enable_entropic", config.enable_entropic);
    extract(node, "enable_filesystem", config.enable_filesystem);
    extract(node, "enable_bash", config.enable_bash);
    extract(node, "enable_git", config.enable_git);
    extract(node, "enable_diagnostics", config.enable_diagnostics);
    extract(node, "enable_web", config.enable_web);
    extract(node, "server_timeout_seconds", config.server_timeout_seconds);
    extract(node, "working_dir", config.working_dir);

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
 * @internal
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
 * @internal
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
 * @brief Parse prompt_cache config from inference YAML section.
 * @param node YAML node for "inference.prompt_cache" section.
 * @param[out] config Output prompt cache config.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.8.3
 */
static std::string parse_prompt_cache_config(
    ryml::ConstNodeRef node,
    PromptCacheConfig& config)
{
    extract(node, "enabled", config.enabled);
    extract(node, "log_hits", config.log_hits);

    int max_bytes_int = 0;
    if (extract(node, "max_bytes", max_bytes_int)) {
        config.max_bytes = static_cast<size_t>(max_bytes_int);
    }

    return "";
}

/**
 * @brief Parse constitutional_validation section.
 * @param node YAML node for "constitutional_validation" section.
 * @param[out] config Output constitutional validation config.
 * @internal
 * @version 2.0.6.1
 */
static void parse_constitutional_validation_config(
    ryml::ConstNodeRef node,
    ConstitutionalValidationConfig& config)
{
    extract(node, "enabled", config.enabled);
    extract(node, "max_revisions", config.max_revisions);
    extract(node, "max_critique_tokens", config.max_critique_tokens);
    extract(node, "temperature", config.temperature);
    extract(node, "enable_thinking", config.enable_thinking);
    extract(node, "priority", config.priority);
    extract(node, "grammar_key", config.grammar_key);
}

/**
 * @brief Parse optional config sections that don't return errors.
 * @param root YAML root node.
 * @param config Config to populate.
 * @internal
 * @version 2.0.6
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
    if (root.has_child("inference") && root["inference"].has_child("prompt_cache"))
        parse_prompt_cache_config(root["inference"]["prompt_cache"],
                                  config.prompt_cache);
    if (root.has_child("constitutional_validation"))
        parse_constitutional_validation_config(
            root["constitutional_validation"],
            config.constitutional_validation);

    extract(root, "log_level", config.log_level);
    extract(root, "inject_model_context", config.inject_model_context);
    extract(root, "vram_reserve_mb", config.vram_reserve_mb);
    extract_path(root, "config_dir", config.config_dir);
    extract_path(root, "log_dir", config.log_dir);
    extract(root, "ggml_logging", config.ggml_logging);

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
 * @brief Load bundled default config when no user config exists.
 * @param global_path Path for auto-created global config.
 * @param registry Bundled models registry.
 * @param[in,out] config Config to populate.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 2.0.0
 */
static std::string load_bundled_default(
    const std::filesystem::path& global_path,
    const BundledModels& registry,
    ParsedConfig& config)
{
    auto data_dir = resolve_data_dir(config);
    auto bundled = data_dir / "default_config.yaml";
    if (!std::filesystem::exists(bundled)) {
        s_log->warn("No config found (checked global, project, bundled)");
        return "";
    }
    s_log->info("No user config — loading bundled default: {}",
                bundled.string());
    auto err = parse_config_file(bundled, registry, config);
    if (!err.empty()) { return "bundled default: " + err; }

    // Auto-create global config so this only happens once
    if (!std::filesystem::exists(global_path)) {
        auto parent = global_path.parent_path();
        std::filesystem::create_directories(parent);
        std::filesystem::copy_file(bundled, global_path);
        s_log->info("Created {}", global_path.string());
    }
    return "";
}

/**
 * @brief Load global, project, and bundled config layers in order.
 * @param global_path Path to global config file.
 * @param project_path Path to project config file.
 * @param registry Bundled models registry.
 * @param[in,out] config Config to overlay onto.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 2.0.0
 */
static std::string load_config_layers(
    const std::filesystem::path& global_path,
    const std::filesystem::path& project_path,
    const BundledModels& registry,
    ParsedConfig& config)
{
    bool have_config = false;
    std::string err;

    if (std::filesystem::exists(global_path)) {
        s_log->info("Loading global config: {}", global_path.string());
        err = parse_config_file(global_path, registry, config);
        if (!err.empty()) { return "global config: " + err; }
        have_config = true;
    }

    if (err.empty() && std::filesystem::exists(project_path)) {
        s_log->info("Loading project config: {}", project_path.string());
        err = parse_config_file(project_path, registry, config);
        if (!err.empty()) { return "project config: " + err; }
        have_config = true;
    }

    return have_config ? ""
        : load_bundled_default(global_path, registry, config);
}

/**
 * @brief Load config using layered resolution.
 * @param global_path Path to global config file.
 * @param project_path Path to project config file.
 * @param registry Bundled models registry.
 * @param[out] config Output parsed config.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 2.0.0
 */
std::string load_config(
    const std::filesystem::path& global_path,
    const std::filesystem::path& project_path,
    const BundledModels& registry,
    ParsedConfig& config)
{
    auto err = load_config_layers(global_path, project_path, registry, config);
    if (!err.empty()) { return err; }

    apply_env_overrides(config);

    std::vector<std::string> warnings;
    err = validate_config(config, warnings);
    for (const auto& w : warnings) { s_log->warn("{}", w); }
    return err;
}

/**
 * @brief Load config from a single YAML file.
 * @param path Path to YAML config file.
 * @param registry Bundled models registry.
 * @param[out] config Output parsed config.
 * @return Empty string on success, error message on failure.
 * @internal
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

/**
 * @brief Parse a single mcpServers entry from .mcp.json.
 * @param name Server name (key in mcpServers object).
 * @param entry JSON value for that server.
 * @return ExternalServerEntry populated from JSON.
 * @utility
 * @version 2.0.3
 */
static ExternalServerEntry parse_mcp_json_entry(
    const std::string& name,
    const nlohmann::json& entry) {
    ExternalServerEntry result;
    std::string type = entry.value("type", std::string("stdio"));
    if (type == "sse") {
        result.url = entry.value("url", std::string{});
    } else {
        result.command = entry.value("command", std::string{});
        if (entry.contains("args") && entry["args"].is_array()) {
            for (const auto& a : entry["args"]) {
                result.args.push_back(a.get<std::string>());
            }
        }
        if (entry.contains("env") && entry["env"].is_object()) {
            for (auto it = entry["env"].begin();
                 it != entry["env"].end(); ++it) {
                result.env[it.key()] = it.value().get<std::string>();
            }
        }
    }
    s_log->info("Discovered external MCP server: {} (type={})",
                 name, type);
    return result;
}

/**
 * @brief Discover and parse .mcp.json from the project directory.
 *
 * Auto-registers external MCP servers (stdio/SSE) declared in the
 * project's .mcp.json. Format matches Claude Code / VSCode MCP config:
 *   { "mcpServers": { "name": { "type": "stdio|sse", ... } } }
 *
 * Entries are merged into config.mcp.external_servers. The
 * ServerManager initializes them during builtin setup.
 *
 * @param project_dir Project directory to search for .mcp.json.
 * @param[in,out] config Config to populate external_servers into.
 * @utility
 * @version 2.0.3
 */
static void discover_mcp_json(
    const std::filesystem::path& project_dir,
    ParsedConfig& config) {
    if (project_dir.empty()) { return; }
    auto path = project_dir / ".mcp.json";
    if (!std::filesystem::exists(path)) { return; }

    std::ifstream f(path);
    nlohmann::json j;
    if (f.is_open()) {
        std::stringstream ss;
        ss << f.rdbuf();
        j = nlohmann::json::parse(ss.str(), nullptr, false);
    }
    bool valid = f.is_open() && !j.is_discarded() && j.is_object()
                 && j.contains("mcpServers")
                 && j["mcpServers"].is_object();
    if (!valid) {
        s_log->warn(".mcp.json missing/malformed: {}", path.string());
        return;
    }

    int added = 0;
    for (auto it = j["mcpServers"].begin();
         it != j["mcpServers"].end(); ++it) {
        const std::string& name = it.key();
        if (config.mcp.external_servers.count(name)) {
            s_log->info(".mcp.json: {} already in config, skipping",
                         name);
            continue;
        }
        config.mcp.external_servers[name] = parse_mcp_json_entry(
            name, it.value());
        added++;
    }
    s_log->info("Loaded {} external MCP server(s) from {}",
                 added, path.string());
}

/**
 * @brief Load the global user config layer if present.
 * @utility
 * @return Empty on success or absence; error message on parse failure.
 * @version 1
 */
static std::string load_global_layer(
    const BundledModels& registry, ParsedConfig& config)
{
    const char* home = getenv("HOME");
    std::string err;
    if (home) {
        auto path = std::filesystem::path(home) / ".entropic" / "config.yaml";
        if (std::filesystem::exists(path)) {
            s_log->info("Loading global config: {}", path.string());
            err = parse_config_file(path, registry, config);
            if (!err.empty()) {
                err = "global config: " + err;
            }
        }
    }
    return err;
}

/**
 * @brief Resolve a relative consumer_defaults path against install prefix.
 *
 * The facade passes a bare filename ("default_config.yaml") that
 * resolves against CWD by default. For tarball-installed consumers
 * whose CWD isn't the install prefix, the bundled
 * <prefix>/share/entropic/<filename> is the correct fallback. Uses
 * the same dladdr-derived discovery as src/config/data_dir.cpp.
 *
 * Returns the input unchanged if it's absolute or already exists.
 * Otherwise attempts <prefix>/share/entropic/<filename>; returns that
 * if found, else returns the original (so the caller's error
 * messages remain sensible).
 *
 * @internal
 * @return Path to use for config loading.
 * @version 1
 */
static std::filesystem::path resolve_consumer_defaults(
    const std::filesystem::path& consumer_defaults)
{
    // Input already usable (empty, absolute, or found at CWD) → passthrough.
    if (consumer_defaults.empty()
        || consumer_defaults.is_absolute()
        || std::filesystem::exists(consumer_defaults)) {
        return consumer_defaults;
    }
    // Fall back to <install-prefix>/share/entropic/<filename>. Uses
    // the same librentropic.so location trick — dladdr on any address
    // in this translation unit resolves to the .so's on-disk path.
    std::filesystem::path result = consumer_defaults;
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(&resolve_consumer_defaults), &info) != 0
        && info.dli_fname != nullptr) {
        std::error_code ec;
        auto lib_path = std::filesystem::absolute(info.dli_fname, ec);
        if (!ec) {
            auto candidate = lib_path.parent_path().parent_path()
                / "share" / "entropic" / consumer_defaults.filename();
            if (std::filesystem::exists(candidate)) {
                result = candidate;
            }
        }
    }
    return result;
}

/**
 * @brief Load the consumer/app defaults layer if present (non-fatal).
 * @utility
 * @version 2
 */
/**
 * @brief Check whether a YAML file contains a top-level key.
 * @param path File path.
 * @param key Key to look for.
 * @return true if the file parses and has the key at root level.
 * @utility
 * @version 2.0.6
 */
static bool yaml_has_key(const std::filesystem::path& path,
                         const char* key) {
    auto content = read_file(path);
    if (content.empty()) { return false; }
    auto tree = ryml::parse_in_arena(
        ryml::to_csubstr(path.string()),
        ryml::to_csubstr(content));
    auto root = tree.rootref();
    return root.is_map() && root.has_child(ryml::to_csubstr(key));
}

/**
 * @brief Load the consumer-defaults layer.
 *
 * Consumer config is a complete application config, not a patch.
 * If the consumer file defines a `models:` block, existing tiers
 * from prior layers are cleared before parsing so the consumer's
 * tier set fully replaces the global one. Same for `routing:`.
 *
 * @param consumer_defaults_in Consumer defaults path.
 * @param registry Bundled models registry.
 * @param[in,out] config Config to overlay onto.
 * @utility
 * @version 2.0.6
 */
static void load_consumer_layer(
    const std::filesystem::path& consumer_defaults_in,
    const BundledModels& registry, ParsedConfig& config)
{
    auto consumer_defaults = resolve_consumer_defaults(consumer_defaults_in);
    if (consumer_defaults.empty() || !std::filesystem::exists(consumer_defaults)) {
        return;
    }
    s_log->info("Loading consumer defaults: {}", consumer_defaults.string());

    // Replace semantics: if consumer defines models or routing,
    // clear existing tiers/routing so global ones don't leak through.
    if (yaml_has_key(consumer_defaults, "models")) {
        s_log->info("Consumer defines models: — replacing global tiers");
        config.models.tiers.clear();
        config.models.router.reset();
        config.models.default_tier.clear();
    }
    if (yaml_has_key(consumer_defaults, "routing")) {
        config.routing = RoutingConfig{};
    }

    auto err = parse_config_file(consumer_defaults, registry, config);
    if (!err.empty()) {
        s_log->warn("Consumer defaults failed: {}", err);
    }
}

/**
 * @brief Load the project-local layer if present.
 * @utility
 * @return Empty on success or absence; error message on parse failure.
 * @version 1
 */
/**
 * @brief Load the project-local layer if present.
 *
 * Same replace semantics as consumer layer: if the project config
 * defines a `models:` or `routing:` block, existing entries from
 * prior layers are cleared before parsing.
 *
 * @utility
 * @return Empty on success or absence; error message on parse failure.
 * @version 2.0.6
 */
static std::string load_project_layer(
    const std::filesystem::path& project_dir,
    const BundledModels& registry, ParsedConfig& config)
{
    std::string err;
    if (!project_dir.empty()) {
        auto path = project_dir / "config.local.yaml";
        if (std::filesystem::exists(path)) {
            s_log->info("Loading project config: {}", path.string());
            if (yaml_has_key(path, "models")) {
                s_log->info("Project defines models: — replacing prior tiers");
                config.models.tiers.clear();
                config.models.router.reset();
                config.models.default_tier.clear();
            }
            if (yaml_has_key(path, "routing")) {
                config.routing = RoutingConfig{};
            }
            err = parse_config_file(path, registry, config);
            if (!err.empty()) {
                err = "project config: " + err;
            }
        }
    }
    return err;
}

/**
 * @brief Load config with global → consumer defaults → project-local layering.
 *
 * Later layers override earlier ones (standard config-precedence semantics:
 * more-specific wins). Order is:
 *
 *   1. Global user config (`~/.entropic/config.yaml`) — personal defaults.
 *   2. Consumer defaults (e.g., an app's bundled `default_config.yaml`) —
 *      the app knows what it needs; overrides the user's global defaults.
 *   3. Project-local (`<project_dir>/config.local.yaml`) — most specific.
 *
 * Prior to v2.0.6 consumer defaults loaded first and were overridden by
 * the user's global config. That meant a user's aggressive global
 * settings (e.g. `context_length: 131072`) would override an app's
 * deliberate lightweight defaults (e.g. `context_length: 16384`) —
 * reversing expected precedence semantics. Now consumer defaults win
 * over global.
 *
 * @param project_dir Project config directory.
 * @param consumer_defaults Path to consumer defaults YAML.
 * @param registry Bundled models registry.
 * @param config Output config.
 * @return Empty string on success.
 * @internal
 * @version 2.0.6
 */
std::string load_layered(
    const std::filesystem::path& project_dir,
    const std::filesystem::path& consumer_defaults,
    const BundledModels& registry,
    ParsedConfig& config)
{
    auto err = load_global_layer(registry, config);
    if (err.empty()) {
        load_consumer_layer(consumer_defaults, registry, config);
        if (!project_dir.empty() && config.log_dir.empty()) {
            config.log_dir = project_dir;
        }
        err = load_project_layer(project_dir, registry, config);
    }
    if (err.empty() && config.models.tiers.empty()) {
        err = load_bundled_default(std::filesystem::path{}, registry, config);
    }
    if (err.empty()) {
        apply_env_overrides(config);
        discover_mcp_json(project_dir, config);
    }
    return err;
}

/**
 * @brief Parse a config string (YAML or JSON) and overlay onto config.
 * @param content Raw config string.
 * @param registry Bundled models for path resolution.
 * @param[in,out] config Config to overlay onto.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 2.0.0
 */
static std::string parse_config_string(
    const std::string& content,
    const BundledModels& registry,
    ParsedConfig& config)
{
    if (content.empty()) {
        return "config string is empty";
    }

    ryml::Tree tree = ryml::parse_in_arena(
        ryml::to_csubstr("<json>"),
        ryml::to_csubstr(content));
    auto root = tree.rootref();
    if (!root.is_map()) {
        return "config string root is not a mapping";
    }

    std::string err;
    if (root.has_child("models")) {
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
 * @brief Load config from a YAML/JSON string with validation.
 * @param content Config string (YAML or JSON).
 * @param registry Bundled models registry.
 * @param[out] config Output parsed config.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 2.0.0
 */
std::string load_config_from_string(
    const std::string& content,
    const BundledModels& registry,
    ParsedConfig& config)
{
    auto err = parse_config_string(content, registry, config);
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
