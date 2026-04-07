/**
 * @file mcp_authorization.cpp
 * @brief MCPAuthorizationManager implementation.
 * @version 1.9.4
 */

#include <entropic/mcp/mcp_authorization.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

static auto logger = entropic::log::get("mcp.authorization");

namespace entropic {

/**
 * @brief Register an empty key set for an identity.
 * @param identity_name Identity/tier name.
 * @internal
 * @version 1.9.4
 */
void MCPAuthorizationManager::register_identity(
    const std::string& identity_name) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    key_sets_.try_emplace(identity_name);
    logger->info("Registered identity: {}", identity_name);
}

/**
 * @brief Check if an identity has enforcement enabled.
 * @param identity_name Identity/tier name.
 * @return true if registered.
 * @internal
 * @version 1.9.4
 */
bool MCPAuthorizationManager::is_enforced(
    const std::string& identity_name) const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    return key_sets_.count(identity_name) > 0;
}

/**
 * @brief Grant a tool key to an identity.
 * @param identity_name Target identity.
 * @param pattern Tool pattern.
 * @param level Access level.
 * @return ENTROPIC_OK or ENTROPIC_ERROR_IDENTITY_NOT_FOUND.
 * @internal
 * @version 1.9.4
 */
entropic_error_t MCPAuthorizationManager::grant(
    const std::string& identity_name,
    const std::string& pattern,
    MCPAccessLevel level) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auto it = key_sets_.find(identity_name);
    if (it == key_sets_.end()) {
        return ENTROPIC_ERROR_IDENTITY_NOT_FOUND;
    }
    it->second.grant(pattern, level);
    return ENTROPIC_OK;
}

/**
 * @brief Revoke a tool key from an identity.
 * @param identity_name Target identity.
 * @param pattern Tool pattern.
 * @return ENTROPIC_OK or ENTROPIC_ERROR_IDENTITY_NOT_FOUND.
 * @internal
 * @version 1.9.4
 */
entropic_error_t MCPAuthorizationManager::revoke(
    const std::string& identity_name,
    const std::string& pattern) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auto it = key_sets_.find(identity_name);
    if (it == key_sets_.end()) {
        return ENTROPIC_ERROR_IDENTITY_NOT_FOUND;
    }
    it->second.revoke(pattern);
    return ENTROPIC_OK;
}

/**
 * @brief Check if a tool call is authorized for an identity.
 * @param identity_name Caller identity.
 * @param tool_name Fully-qualified tool name.
 * @param required_level Minimum access level.
 * @return true if authorized or no enforcement.
 * @internal
 * @version 2.0.0
 */
bool MCPAuthorizationManager::check_access(
    const std::string& identity_name,
    const std::string& tool_name,
    MCPAccessLevel required_level) const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auto it = key_sets_.find(identity_name);
    if (it == key_sets_.end()) {
        logger->info("Auth ALLOW: {} on {} (no enforcement)",
                     identity_name, tool_name);
        return true;
    }
    bool allowed = it->second.has_access(tool_name, required_level);
    logger->info("Auth {}: {} on {} (required={})",
                 allowed ? "ALLOW" : "DENY",
                 identity_name, tool_name,
                 static_cast<int>(required_level));
    return allowed;
}

/**
 * @brief One identity grants a key to another.
 * @param granter_name Granting identity.
 * @param grantee_name Receiving identity.
 * @param pattern Tool pattern.
 * @param level Access level.
 * @return ENTROPIC_OK, NOT_FOUND, or PERMISSION_DENIED.
 * @internal
 * @version 1.9.4
 */
entropic_error_t MCPAuthorizationManager::grant_from(
    const std::string& granter_name,
    const std::string& grantee_name,
    const std::string& pattern,
    MCPAccessLevel level) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auto granter = key_sets_.find(granter_name);
    auto grantee = key_sets_.find(grantee_name);
    if (granter == key_sets_.end() ||
        grantee == key_sets_.end()) {
        return ENTROPIC_ERROR_IDENTITY_NOT_FOUND;
    }
    if (!granter->second.has_access(pattern, level)) {
        logger->warn("{} lacks {}:{} — cannot grant to {}",
                     granter_name, pattern,
                     mcp_access_level_name(level), grantee_name);
        return ENTROPIC_ERROR_PERMISSION_DENIED;
    }
    grantee->second.grant(pattern, level);
    logger->info("{} granted {}:{} to {}", granter_name, pattern,
                 mcp_access_level_name(level), grantee_name);
    return ENTROPIC_OK;
}

/**
 * @brief List all keys for an identity.
 * @param identity_name Identity to query.
 * @return Vector of MCPKey entries.
 * @internal
 * @version 1.9.4
 */
std::vector<MCPKey> MCPAuthorizationManager::list_keys(
    const std::string& identity_name) const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auto it = key_sets_.find(identity_name);
    if (it == key_sets_.end()) {
        return {};
    }
    return it->second.list();
}

/**
 * @brief Serialize all identity key sets to JSON.
 * @return JSON object string.
 * @internal
 * @version 1.9.4
 */
std::string MCPAuthorizationManager::serialize_all() const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [name, key_set] : key_sets_) {
        auto keys_json = key_set.serialize();
        obj[name] = nlohmann::json::parse(keys_json);
    }
    return obj.dump();
}

/**
 * @brief Deserialize all identity key sets from JSON.
 * @param json JSON object string.
 * @return true if parsed successfully.
 * @internal
 * @version 1.9.4
 */
bool MCPAuthorizationManager::deserialize_all(
    const std::string& json) {
    nlohmann::json obj;
    try {
        obj = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& e) {
        logger->warn("Deserialize failed: {}", e.what());
        return false;
    }
    if (!obj.is_object()) {
        logger->warn("Deserialize: expected JSON object");
        return false;
    }
    std::lock_guard<std::mutex> lock(auth_mutex_);
    key_sets_.clear();
    for (const auto& [name, keys_arr] : obj.items()) {
        auto [it, _] = key_sets_.try_emplace(name);
        it->second.deserialize(keys_arr.dump());
    }
    return true;
}

/**
 * @brief Remove an identity's key set.
 * @param identity_name Identity to unregister.
 * @internal
 * @version 1.9.4
 */
void MCPAuthorizationManager::unregister_identity(
    const std::string& identity_name) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    key_sets_.erase(identity_name);
    logger->info("Unregistered identity: {}", identity_name);
}

} // namespace entropic
