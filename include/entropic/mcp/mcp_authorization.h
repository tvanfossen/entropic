// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file mcp_authorization.h
 * @brief Per-identity MCP authorization with runtime grant/revoke.
 *
 * Manages MCPKeySet instances per identity. Provides the enforcement
 * check called by ToolExecutor before tool execution. Handles
 * cross-identity key granting (identity A grants keys to identity B).
 *
 * @par Ownership
 * One MCPAuthorizationManager per engine instance.
 *
 * @par Default behavior
 * If an identity has no MCPKeySet registered, the authorization check
 * PASSES (backward compatible — v1.8.5/v1.8.6 behavior). Once a key
 * set is registered for an identity (even an empty one), authorization
 * is enforced. This allows incremental adoption.
 *
 * @par Thread safety
 * All public methods acquire auth_mutex_.
 *
 * @version 1.9.4
 */

#pragma once

#include <entropic/mcp/mcp_key_set.h>
#include <entropic/types/error.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Per-identity MCP authorization with runtime grant/revoke.
 * @version 1.9.4
 */
class MCPAuthorizationManager {
public:
    /**
     * @brief Register an empty key set for an identity.
     * @param identity_name Identity/tier name.
     * @version 1.9.4
     *
     * After registration, the identity has ZERO authorized tools.
     */
    void register_identity(const std::string& identity_name);

    /**
     * @brief Check if an identity has authorization enforcement enabled.
     * @param identity_name Identity/tier name.
     * @return true if the identity has a registered MCPKeySet.
     * @version 1.9.4
     */
    bool is_enforced(const std::string& identity_name) const;

    /**
     * @brief Grant a tool key to an identity.
     * @param identity_name Target identity.
     * @param pattern Tool pattern string.
     * @param level Access level to grant.
     * @return ENTROPIC_OK on success.
     *         ENTROPIC_ERROR_IDENTITY_NOT_FOUND if identity not registered.
     * @version 1.9.4
     */
    entropic_error_t grant(const std::string& identity_name,
                           const std::string& pattern,
                           MCPAccessLevel level);

    /**
     * @brief Revoke a tool key from an identity.
     * @param identity_name Target identity.
     * @param pattern Tool pattern string.
     * @return ENTROPIC_OK on success.
     *         ENTROPIC_ERROR_IDENTITY_NOT_FOUND if identity not registered.
     * @version 1.9.4
     */
    entropic_error_t revoke(const std::string& identity_name,
                            const std::string& pattern);

    /**
     * @brief Check if a tool call is authorized for an identity.
     * @param identity_name Caller identity.
     * @param tool_name Fully-qualified tool name.
     * @param required_level Minimum access level needed.
     * @return true if authorized (or if identity has no key set).
     * @version 1.9.4
     */
    bool check_access(const std::string& identity_name,
                      const std::string& tool_name,
                      MCPAccessLevel required_level) const;

    /**
     * @brief One identity grants a key to another identity.
     * @param granter_name Identity performing the grant.
     * @param grantee_name Identity receiving the key.
     * @param pattern Tool pattern to grant.
     * @param level Access level to grant.
     * @return ENTROPIC_OK on success.
     *         ENTROPIC_ERROR_IDENTITY_NOT_FOUND if either not registered.
     *         ENTROPIC_ERROR_PERMISSION_DENIED if granter lacks the key.
     * @version 1.9.4
     */
    entropic_error_t grant_from(const std::string& granter_name,
                                const std::string& grantee_name,
                                const std::string& pattern,
                                MCPAccessLevel level);

    /**
     * @brief List all keys for an identity.
     * @param identity_name Identity to query.
     * @return Vector of MCPKey entries, or empty if not registered.
     * @version 1.9.4
     */
    std::vector<MCPKey> list_keys(
        const std::string& identity_name) const;

    /**
     * @brief Serialize all identity key sets to JSON.
     * @return JSON object: {"identity_name": [key_array], ...}
     * @version 1.9.4
     */
    std::string serialize_all() const;

    /**
     * @brief Deserialize all identity key sets from JSON.
     * @param json JSON object string.
     * @return true if parsed successfully.
     * @version 1.9.4
     */
    bool deserialize_all(const std::string& json);

    /**
     * @brief Remove an identity's key set (disables enforcement).
     * @param identity_name Identity to unregister.
     * @version 1.9.4
     */
    void unregister_identity(const std::string& identity_name);

private:
    /// @brief Key sets per identity.
    std::unordered_map<std::string, MCPKeySet> key_sets_;

    /// @brief Guards mutations to key_sets_ map.
    mutable std::mutex auth_mutex_;
};

} // namespace entropic
