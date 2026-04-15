// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file identity_manager.cpp
 * @brief IdentityManager implementation -- CRUD, validation, MCP key lifecycle.
 * @version 1.9.6
 */

#include <entropic/core/identity_manager.h>
#include <entropic/types/logging.h>

#include <algorithm>
#include <filesystem>
#include <regex>

namespace entropic {

static auto logger = log::get("identity_manager");

/// @brief Reserved identity names that cannot be used.
static const std::vector<std::string> s_reserved_names = {
    "default", "none", "all", "router"};

/// @brief Valid role_type values.
static const std::vector<std::string> s_valid_role_types = {
    "front_office", "back_office", "utility"};

/**
 * @brief Compiled regex for identity name validation.
 * @internal
 * @version 1.9.6
 */
static const std::regex s_name_regex("^[a-z][a-z0-9_-]{0,63}$");

// ── Phase validation (extracted for return count) ────────

/**
 * @brief Validate all phases in an identity config.
 * @param phases Phase map to validate.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.9.9
 */
static std::string validate_phases(
    const std::unordered_map<std::string,
                             PhaseConfig>& phases) {
    for (const auto& [phase_name, phase] : phases) {
        if (phase_name.empty()) {
            return "Phase name cannot be empty";
        }
        if (phase.max_output_tokens <= 0) {
            return "Phase '" + phase_name
                   + "' max_output_tokens must be > 0";
        }
    }
    return "";
}

// ── Field validation ─────────────────────────────────────

/**
 * @brief Validate role_type, phases, and adapter_path fields.
 * @param config Config to validate.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.9.6
 */
static std::string validate_fields(const IdentityConfig& config) {
    auto rt = std::find(
        s_valid_role_types.begin(), s_valid_role_types.end(),
        config.role_type);
    if (rt == s_valid_role_types.end()) {
        return "Invalid role_type: " + config.role_type;
    }
    auto phase_err = validate_phases(config.phases);
    if (!phase_err.empty()) { return phase_err; }
    bool bad_adapter = !config.adapter_path.empty()
        && !std::filesystem::exists(config.adapter_path);
    return bad_adapter
        ? "Adapter path not found: " + config.adapter_path : "";
}

// ── Create precondition check (extracted for return count) ──

/**
 * @brief Check create preconditions before validation.
 * @param config Identity config to check.
 * @param mgr_config Manager configuration.
 * @param identities Current identity map.
 * @return ENTROPIC_OK if preconditions pass, error code otherwise.
 * @internal
 * @version 1.9.6
 */
static entropic_error_t check_create_preconditions(
    const IdentityConfig& config,
    const IdentityManagerConfig& mgr_config,
    const std::unordered_map<std::string, IdentityConfig>& identities) {
    if (!mgr_config.allow_dynamic) {
        logger->warn("Dynamic identity creation disabled");
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }
    if (identities.size() >= mgr_config.max_identities) {
        logger->warn("Identity limit reached: {}",
                     mgr_config.max_identities);
        return ENTROPIC_ERROR_LIMIT_REACHED;
    }
    bool exists = identities.count(config.name) > 0;
    if (exists) {
        logger->warn("Identity already exists: {}", config.name);
    }
    return exists ? ENTROPIC_ERROR_ALREADY_EXISTS : ENTROPIC_OK;
}

// ── Mutation precondition check (update/destroy) ─────────

/**
 * @brief Check that a mutable identity exists and is dynamic.
 * @param name Identity name.
 * @param identities Identity map.
 * @return Iterator to identity on success, end() on failure.
 *         Sets out_err to the error code on failure.
 * @internal
 * @version 1.9.6
 */
static entropic_error_t check_mutable(
    const std::string& name,
    const std::unordered_map<std::string, IdentityConfig>& identities,
    std::unordered_map<std::string,
                       IdentityConfig>::const_iterator& out_it) {
    out_it = identities.find(name);
    if (out_it == identities.end()) {
        return ENTROPIC_ERROR_IDENTITY_NOT_FOUND;
    }
    return (out_it->second.origin == IdentityOrigin::STATIC)
        ? ENTROPIC_ERROR_PERMISSION_DENIED : ENTROPIC_OK;
}

// ── Constructor ──────────────────────────────────────────

/**
 * @brief Construct with configuration.
 * @param config Identity manager configuration.
 * @internal
 * @version 1.9.6
 */
IdentityManager::IdentityManager(const IdentityManagerConfig& config)
    : config_(config) {}

// ── Interface setters ────────────────────────────────────

/**
 * @brief Set grammar validation interface.
 * @param iface Grammar validation callbacks.
 * @internal
 * @version 1.9.6
 */
void IdentityManager::set_grammar_interface(
    const GrammarValidationInterface& iface) {
    grammar_iface_ = iface;
}

/**
 * @brief Set MCP key management interface.
 * @param iface MCP key callbacks.
 * @internal
 * @version 1.9.6
 */
void IdentityManager::set_mcp_interface(const MCPKeyInterface& iface) {
    mcp_iface_ = iface;
}

// ── load_static ──────────────────────────────────────────

/**
 * @brief Load static identities from config loader.
 * @param identities Vector of static identity configs.
 * @return Number of identities loaded.
 * @internal
 * @version 1.9.6
 */
size_t IdentityManager::load_static(
    const std::vector<IdentityConfig>& identities) {
    std::unique_lock lock(identities_mutex_);
    size_t loaded = 0;
    for (const auto& id : identities) {
        IdentityConfig cfg = id;
        cfg.origin = IdentityOrigin::STATIC;
        identities_[cfg.name] = std::move(cfg);
        ++loaded;
    }
    logger->info("Loaded {} static identities", loaded);
    router_dirty_.store(true, std::memory_order_release);
    return loaded;
}

// ── create ───────────────────────────────────────────────

/**
 * @brief Create a new dynamic identity.
 * @param config Identity configuration.
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 2.0.0
 */
entropic_error_t IdentityManager::create(const IdentityConfig& config) {
    std::unique_lock lock(identities_mutex_);
    auto pre = check_create_preconditions(
        config, config_, identities_);
    if (pre != ENTROPIC_OK) { return pre; }
    auto err = validate(config, false);
    if (!err.empty()) {
        logger->warn("Identity validation failed: {}", err);
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }
    IdentityConfig cfg = config;
    cfg.origin = IdentityOrigin::DYNAMIC;
    register_mcp_keys(cfg);
    identities_[cfg.name] = std::move(cfg);
    router_dirty_.store(true, std::memory_order_release);
    logger->info("Identity created: name='{}', routable={}, "
                 "prompt={} chars",
                 config.name, config.routable,
                 config.system_prompt.size());
    return ENTROPIC_OK;
}

// ── update ───────────────────────────────────────────────

/**
 * @brief Update an existing dynamic identity.
 * @param name Identity name to update.
 * @param config New configuration (name field must match).
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 1.9.6
 */
entropic_error_t IdentityManager::update(
    const std::string& name,
    const IdentityConfig& config) {
    std::unique_lock lock(identities_mutex_);
    std::unordered_map<std::string,
        IdentityConfig>::const_iterator it;
    auto pre = check_mutable(name, identities_, it);
    if (pre != ENTROPIC_OK) { return pre; }
    auto err = validate(config, true);
    if (!err.empty()) {
        logger->warn("Identity update validation failed: {}", err);
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }
    unregister_mcp_keys(name);
    IdentityConfig cfg = config;
    cfg.origin = IdentityOrigin::DYNAMIC;
    cfg.name = name;
    register_mcp_keys(cfg);
    identities_[name] = std::move(cfg);
    router_dirty_.store(true, std::memory_order_release);
    logger->info("Dynamic identity updated: {}", name);
    return ENTROPIC_OK;
}

// ── destroy ──────────────────────────────────────────────

/**
 * @brief Destroy a dynamic identity.
 * @param name Identity name to destroy.
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 1.9.6
 */
entropic_error_t IdentityManager::destroy(const std::string& name) {
    std::unique_lock lock(identities_mutex_);
    std::unordered_map<std::string,
        IdentityConfig>::const_iterator it;
    auto pre = check_mutable(name, identities_, it);
    if (pre != ENTROPIC_OK) { return pre; }
    if (in_use_checker_ && in_use_checker_(name, in_use_user_data_)) {
        return ENTROPIC_ERROR_IN_USE;
    }
    unregister_mcp_keys(name);
    identities_.erase(it);
    router_dirty_.store(true, std::memory_order_release);
    logger->info("Dynamic identity destroyed: {}", name);
    return ENTROPIC_OK;
}

// ── get ──────────────────────────────────────────────────

/**
 * @brief Get identity config by name.
 * @param name Identity name.
 * @return Pointer to config, or nullptr if not found.
 * @internal
 * @version 1.9.6
 */
const IdentityConfig* IdentityManager::get(
    const std::string& name) const {
    std::shared_lock lock(identities_mutex_);
    auto it = identities_.find(name);
    return (it != identities_.end()) ? &it->second : nullptr;
}

// ── has ──────────────────────────────────────────────────

/**
 * @brief Check if an identity exists.
 * @param name Identity name.
 * @return true if identity exists.
 * @internal
 * @version 1.9.6
 */
bool IdentityManager::has(const std::string& name) const {
    std::shared_lock lock(identities_mutex_);
    return identities_.count(name) > 0;
}

// ── list ─────────────────────────────────────────────────

/**
 * @brief List all identity names.
 * @return Vector of identity names (static + dynamic).
 * @internal
 * @version 1.9.6
 */
std::vector<std::string> IdentityManager::list() const {
    std::shared_lock lock(identities_mutex_);
    std::vector<std::string> names;
    names.reserve(identities_.size());
    for (const auto& [k, _] : identities_) {
        names.push_back(k);
    }
    return names;
}

// ── list_routable ────────────────────────────────────────

/**
 * @brief List only routable identities for classification prompt.
 * @return Vector of pointers to routable identity configs.
 * @internal
 * @version 1.9.6
 */
std::vector<const IdentityConfig*>
IdentityManager::list_routable() const {
    std::shared_lock lock(identities_mutex_);
    std::vector<const IdentityConfig*> result;
    for (const auto& [_, cfg] : identities_) {
        if (cfg.routable && !cfg.interstitial) {
            result.push_back(&cfg);
        }
    }
    return result;
}

// ── count / count_dynamic ────────────────────────────────

/**
 * @brief Get the total number of identities.
 * @return Total count (static + dynamic).
 * @internal
 * @version 1.9.6
 */
size_t IdentityManager::count() const {
    std::shared_lock lock(identities_mutex_);
    return identities_.size();
}

/**
 * @brief Get the number of dynamic identities.
 * @return Dynamic identity count only.
 * @internal
 * @version 1.9.6
 */
size_t IdentityManager::count_dynamic() const {
    std::shared_lock lock(identities_mutex_);
    size_t n = 0;
    for (const auto& [_, cfg] : identities_) {
        if (cfg.origin == IdentityOrigin::DYNAMIC) {
            ++n;
        }
    }
    return n;
}

// ── Router dirty flag ────────────────────────────────────

/**
 * @brief Check if the router classification prompt needs rebuilding.
 * @return true if identity mutations have occurred since last clear.
 * @internal
 * @version 1.9.6
 */
bool IdentityManager::is_router_dirty() const {
    return router_dirty_.load(std::memory_order_acquire);
}

/**
 * @brief Clear the dirty flag.
 * @internal
 * @version 1.9.6
 */
void IdentityManager::clear_router_dirty() {
    router_dirty_.store(false, std::memory_order_release);
}

// ── In-use checker ───────────────────────────────────────

/**
 * @brief Set the in-use checker callback.
 * @param checker Function returning true if name is in active delegation.
 * @param user_data Opaque pointer forwarded to checker.
 * @internal
 * @version 1.9.6
 */
void IdentityManager::set_in_use_checker(
    bool (*checker)(const std::string& name, void* user_data),
    void* user_data) {
    in_use_checker_ = checker;
    in_use_user_data_ = user_data;
}

// ── validate ─────────────────────────────────────────────

/**
 * @brief Validate identity name format.
 * @param name Name to validate.
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.9.6
 */
std::string IdentityManager::validate_name(const std::string& name) {
    if (name.empty()) {
        return "Identity name cannot be empty";
    }
    if (!std::regex_match(name, s_name_regex)) {
        return "Identity name must match ^[a-z][a-z0-9_-]{0,63}$";
    }
    bool reserved = std::find(
        s_reserved_names.begin(), s_reserved_names.end(), name)
        != s_reserved_names.end();
    return reserved
        ? "Identity name '" + name + "' is reserved" : "";
}

/**
 * @brief Validate an identity config.
 * @param config Config to validate.
 * @param is_update true if this is an update (name already exists).
 * @return Empty string on success, error message on failure.
 * @internal
 * @version 1.9.6
 */
std::string IdentityManager::validate(
    const IdentityConfig& config, bool is_update) const {
    if (!is_update) {
        auto name_err = validate_name(config.name);
        if (!name_err.empty()) { return name_err; }
    }
    if (config.focus.empty()) {
        return "Identity must have at least one focus keyword";
    }
    bool grammar_missing = !config.grammar_id.empty()
        && grammar_iface_.has_grammar
        && !grammar_iface_.has_grammar(
               config.grammar_id, grammar_iface_.user_data);
    return grammar_missing
        ? "Grammar '" + config.grammar_id + "' not found in registry"
        : validate_fields(config);
}

// ── MCP key registration ─────────────────────────────────

/**
 * @brief Register MCP keys for an identity via callback interface.
 * @param config Identity config with mcp_keys.
 * @internal
 * @version 1.9.6
 */
void IdentityManager::register_mcp_keys(const IdentityConfig& config) {
    if (config.mcp_keys.empty()) { return; }
    if (!mcp_iface_.register_identity) { return; }
    mcp_iface_.register_identity(config.name, mcp_iface_.user_data);
    for (const auto& key : config.mcp_keys) {
        if (mcp_iface_.grant) {
            mcp_iface_.grant(config.name, key.tool_pattern,
                             static_cast<int>(key.level),
                             mcp_iface_.user_data);
        }
    }
}

/**
 * @brief Unregister MCP keys for an identity via callback interface.
 * @param name Identity name.
 * @internal
 * @version 1.9.6
 */
void IdentityManager::unregister_mcp_keys(const std::string& name) {
    if (!mcp_iface_.is_enforced || !mcp_iface_.unregister_identity) {
        return;
    }
    if (mcp_iface_.is_enforced(name, mcp_iface_.user_data)) {
        mcp_iface_.unregister_identity(name, mcp_iface_.user_data);
    }
}

} // namespace entropic
