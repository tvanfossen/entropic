/**
 * @file profile_registry.h
 * @brief ProfileRegistry -- named GPU resource profile management.
 *
 * @par Responsibilities:
 * - Store bundled profiles (maximum, balanced, background, minimal)
 * - Register/deregister custom profiles at runtime
 * - Resolve profile name to GPUResourceProfile struct
 *
 * @par Thread safety:
 * - All public methods acquire mutex_ (registry is small, contention minimal)
 * - get() on unknown name logs WARNING and falls back to "balanced"
 *
 * @par Ownership:
 * Owned by ModelOrchestrator. One registry per engine instance.
 * Same single-class pattern as GrammarRegistry (design decision #31).
 *
 * @version 1.9.7
 */

#pragma once

#include <entropic/types/config.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Centralized registry for named GPU resource profiles.
 *
 * Single concrete class (no three-layer hierarchy -- one implementation,
 * like GrammarRegistry, AdapterManager, and HookRegistry).
 *
 * @version 1.9.7
 */
class ProfileRegistry {
public:
    /**
     * @brief Initialize with bundled profiles (maximum, balanced, background, minimal).
     *
     * Computes hardware-dependent thread counts at call time:
     * - background profile uses max(1, hardware_concurrency/2)
     * - Falls back to n_threads=2 if hardware_concurrency() returns 0
     *
     * Safe to call multiple times (idempotent -- clears and reloads).
     *
     * @version 1.9.7
     */
    void load_bundled();

    /**
     * @brief Register a custom profile.
     * @param profile Profile to register. profile.name is the key.
     * @return true on success. false if name already exists.
     * @version 1.9.7
     */
    bool register_profile(const GPUResourceProfile& profile);

    /**
     * @brief Remove a profile by name.
     * @param name Profile name.
     * @return true if removed. false if not found.
     * @version 1.9.7
     */
    bool deregister(const std::string& name);

    /**
     * @brief Get a profile by name.
     * @param name Profile name.
     * @return Profile struct. Returns the "balanced" profile if name not found
     *         (with WARNING log). Returns default-constructed profile if
     *         "balanced" also missing (should never happen after load_bundled).
     * @version 1.9.7
     */
    GPUResourceProfile get(const std::string& name) const;

    /**
     * @brief Check if a profile name exists.
     * @param name Profile name.
     * @return true if registered.
     * @version 1.9.7
     */
    bool has(const std::string& name) const;

    /**
     * @brief List all registered profile names.
     * @return Sorted vector of profile names.
     * @version 1.9.7
     */
    std::vector<std::string> list() const;

    /**
     * @brief Number of registered profiles.
     * @return Count of profiles in the registry.
     * @version 1.9.7
     */
    size_t size() const;

private:
    /// @brief All registered profiles, keyed by name.
    std::unordered_map<std::string, GPUResourceProfile> profiles_;

    /// @brief Guards all mutations and reads to profiles_ map.
    mutable std::mutex mutex_;
};

} // namespace entropic
