// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file identity_manager.h
 * @brief IdentityManager -- lifecycle management for static and dynamic identities.
 *
 * @par Responsibilities:
 * - Own all IdentityConfig instances (static and dynamic)
 * - Enforce max_identities limit
 * - Validate identity configs at creation time
 * - Coordinate with GrammarRegistry and MCPAuthorizationManager via callbacks
 * - Flag router classification prompt for rebuild when identities change
 * - Provide identity lookup by name for engine, orchestrator, delegation
 *
 * @par Thread safety:
 * - get() and has() use shared_lock (concurrent reads)
 * - create/update/destroy acquire unique_lock on identities_mutex_
 * - Router dirty flag is std::atomic<bool> (lock-free)
 *
 * @par Cross-.so design:
 * Core.so has zero dependencies on inference or mcp. Grammar validation
 * and MCP key registration are injected as callback interfaces (same
 * pattern as TierResolutionInterface, ToolExecutionInterface).
 *
 * @par Ownership:
 * Owned by AgentEngine. One IdentityManager per engine instance.
 *
 * @version 1.9.6
 */

#pragma once

#include <entropic/config/identity.h>
#include <entropic/types/error.h>

#include <atomic>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Configuration for the identity manager.
 * @version 1.9.6
 */
struct IdentityManagerConfig {
    size_t max_identities = 64;  ///< Maximum total identities (static + dynamic)
    bool allow_dynamic = true;   ///< Master toggle for dynamic identity creation
};

/**
 * @brief Grammar validation callbacks injected by the facade.
 *
 * Core.so cannot depend on inference.so. The facade wires these
 * to GrammarRegistry at engine initialization.
 *
 * @version 1.9.6
 */
struct GrammarValidationInterface {
    /// @brief Check if a grammar key exists in the registry.
    bool (*has_grammar)(const std::string& key,
                        void* user_data) = nullptr;
    void* user_data = nullptr; ///< Opaque pointer (GrammarRegistry*)
};

/**
 * @brief MCP key management callbacks injected by the facade.
 *
 * Core.so cannot depend on mcp.so. The facade wires these to
 * MCPAuthorizationManager at engine initialization.
 *
 * @version 1.9.6
 */
struct MCPKeyInterface {
    /// @brief Register an empty key set for an identity.
    void (*register_identity)(const std::string& name,
                              void* user_data) = nullptr;
    /// @brief Grant a key pattern to an identity.
    void (*grant)(const std::string& name, const std::string& pattern,
                  int level, void* user_data) = nullptr;
    /// @brief Check if an identity has a key set registered.
    bool (*is_enforced)(const std::string& name,
                        void* user_data) = nullptr;
    /// @brief Remove an identity's key set.
    void (*unregister_identity)(const std::string& name,
                                void* user_data) = nullptr;
    void* user_data = nullptr; ///< Opaque pointer (MCPAuthorizationManager*)
};

/**
 * @brief Manages the lifecycle of all identities in the engine.
 *
 * @par Lifecycle:
 * @code
 *   IdentityManager mgr(config);
 *   mgr.set_grammar_interface(grammar_iface);
 *   mgr.set_mcp_interface(mcp_iface);
 *   mgr.load_static(static_identities);
 *   mgr.create(dynamic_config);
 *   auto* id = mgr.get("npc_guard");
 *   mgr.update("npc_guard", new_config);
 *   mgr.destroy("npc_guard");
 * @endcode
 *
 * @version 1.9.6
 */
class IdentityManager {
public:
    /**
     * @brief Construct with configuration.
     * @param config Identity manager configuration.
     * @version 1.9.6
     */
    explicit IdentityManager(const IdentityManagerConfig& config);

    /**
     * @brief Set grammar validation interface.
     * @param iface Grammar validation callbacks.
     * @version 1.9.6
     */
    void set_grammar_interface(const GrammarValidationInterface& iface);

    /**
     * @brief Set MCP key management interface.
     * @param iface MCP key callbacks.
     * @version 1.9.6
     */
    void set_mcp_interface(const MCPKeyInterface& iface);

    /**
     * @brief Load static identities from config loader.
     * @param identities Vector of static identity configs.
     * @return Number of identities loaded.
     * @version 1.9.6
     *
     * Called once at engine startup. Static identities have
     * origin=STATIC and cannot be destroyed via destroy().
     */
    size_t load_static(const std::vector<IdentityConfig>& identities);

    /**
     * @brief Create a new dynamic identity.
     * @param config Identity configuration.
     * @return ENTROPIC_OK on success.
     *         ENTROPIC_ERROR_INVALID_CONFIG if validation fails.
     *         ENTROPIC_ERROR_LIMIT_REACHED if max_identities exceeded.
     *         ENTROPIC_ERROR_ALREADY_EXISTS if name already taken.
     * @version 1.9.6
     */
    entropic_error_t create(const IdentityConfig& config);

    /**
     * @brief Update an existing dynamic identity.
     * @param name Identity name to update.
     * @param config New configuration (name field must match).
     * @return ENTROPIC_OK on success.
     *         ENTROPIC_ERROR_IDENTITY_NOT_FOUND if identity doesn't exist.
     *         ENTROPIC_ERROR_PERMISSION_DENIED if identity is static.
     *         ENTROPIC_ERROR_INVALID_CONFIG if validation fails.
     * @version 1.9.6
     */
    entropic_error_t update(const std::string& name,
                            const IdentityConfig& config);

    /**
     * @brief Destroy a dynamic identity.
     * @param name Identity name to destroy.
     * @return ENTROPIC_OK on success.
     *         ENTROPIC_ERROR_IDENTITY_NOT_FOUND if identity doesn't exist.
     *         ENTROPIC_ERROR_PERMISSION_DENIED if identity is static.
     *         ENTROPIC_ERROR_IN_USE if identity is currently active in
     *         a delegation (locked_tier matches name).
     * @version 1.9.6
     */
    entropic_error_t destroy(const std::string& name);

    /**
     * @brief Get identity config by name.
     * @param name Identity name.
     * @return Pointer to config, or nullptr if not found.
     * @version 1.9.6
     *
     * Pointer valid until identity is destroyed or updated.
     * Read via shared_lock.
     */
    const IdentityConfig* get(const std::string& name) const;

    /**
     * @brief Check if an identity exists.
     * @param name Identity name.
     * @return true if identity exists.
     * @version 1.9.6
     */
    bool has(const std::string& name) const;

    /**
     * @brief List all identity names.
     * @return Vector of identity names (static + dynamic).
     * @version 1.9.6
     */
    std::vector<std::string> list() const;

    /**
     * @brief List only routable identities (for classification prompt).
     * @return Vector of pointers to routable identity configs.
     * @version 1.9.6
     *
     * Routable = routable==true && interstitial==false.
     * Pointers valid until next create/update/destroy.
     */
    std::vector<const IdentityConfig*> list_routable() const;

    /**
     * @brief Get the total number of identities.
     * @return Total count (static + dynamic).
     * @version 1.9.6
     */
    size_t count() const;

    /**
     * @brief Get the number of dynamic identities.
     * @return Dynamic identity count only.
     * @version 1.9.6
     */
    size_t count_dynamic() const;

    /**
     * @brief Check if the router classification prompt needs rebuilding.
     * @return true if create/update/destroy has been called since last clear.
     * @version 1.9.6
     */
    bool is_router_dirty() const;

    /**
     * @brief Clear the dirty flag (called after router prompt rebuild).
     * @version 1.9.6
     */
    void clear_router_dirty();

    /**
     * @brief Set the in-use checker callback.
     * @param checker Function returning true if name is in active delegation.
     * @param user_data Opaque pointer forwarded to checker.
     * @version 1.9.6
     */
    void set_in_use_checker(bool (*checker)(const std::string& name,
                                            void* user_data),
                            void* user_data);

private:
    /**
     * @brief Validate an identity config.
     * @param config Config to validate.
     * @param is_update true if this is an update (name must already exist).
     * @return Empty string on success, error message on failure.
     * @version 1.9.6
     */
    std::string validate(const IdentityConfig& config,
                         bool is_update) const;

    /**
     * @brief Validate identity name format.
     * @param name Name to validate.
     * @return Empty string on success, error message on failure.
     * @version 1.9.6
     */
    static std::string validate_name(const std::string& name);

    /**
     * @brief Register MCP keys for an identity via callback interface.
     * @param config Identity config with mcp_keys.
     * @version 1.9.6
     */
    void register_mcp_keys(const IdentityConfig& config);

    /**
     * @brief Unregister MCP keys for an identity via callback interface.
     * @param name Identity name.
     * @version 1.9.6
     */
    void unregister_mcp_keys(const std::string& name);

    IdentityManagerConfig config_;                              ///< Manager configuration
    GrammarValidationInterface grammar_iface_;                 ///< Grammar validation callbacks
    MCPKeyInterface mcp_iface_;                                ///< MCP key management callbacks

    std::unordered_map<std::string, IdentityConfig> identities_; ///< All identities keyed by name
    mutable std::shared_mutex identities_mutex_;               ///< Guards mutations to identities_ map

    std::atomic<bool> router_dirty_{false};                    ///< Router classification prompt needs rebuild

    bool (*in_use_checker_)(const std::string&, void*) = nullptr; ///< In-use check callback
    void* in_use_user_data_ = nullptr;                            ///< In-use check opaque pointer
};

} // namespace entropic
