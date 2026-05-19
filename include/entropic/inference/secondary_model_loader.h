// SPDX-License-Identifier: Apache-2.0
/**
 * @file secondary_model_loader.h
 * @brief Unified lifecycle for non-primary inference backends.
 *
 * Owns role-keyed `std::shared_ptr<InferenceBackend>` slots for models
 * that are not part of the main-tier pool: the router (always-ACTIVE
 * classifier), the speculative draft (CPU- or GPU-resident, v2.1.11+),
 * and the future thinking-model (gh#25). Each role is loaded lazily on
 * first use, unloaded via `release_role()`, and survives independent of
 * the main-tier swap path.
 *
 * Follows the single-class pattern of AdapterManager (architecture
 * decision #29) and GrammarRegistry (decision #31): one implementation,
 * no interface layer. Owned by ModelOrchestrator via composition. Not
 * exposed across the inference `.so` boundary — all callers are
 * inference-internal.
 *
 * @par Thread safety
 * Lifecycle transitions (ensure_loaded / release_role / shutdown)
 * acquire an internal mutex. `get()` is lock-free once a role has been
 * loaded — same atomic-state contract as InferenceBackend itself.
 *
 * @version 2.1.11
 */

#pragma once

#include <entropic/inference/backend.h>
#include <entropic/types/config.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Role-keyed lifecycle manager for non-primary models.
 *
 * Replaces the per-role `std::shared_ptr<InferenceBackend>` members
 * (`router_`) that previously lived directly on ModelOrchestrator. The
 * router refactor is intentionally invisible to callers: existing
 * router behavior is preserved via `loader_.get("router")`.
 *
 * @par Role names (conventional, not enforced):
 * - `"router"` — digit-classifier model used by ModelOrchestrator::route()
 * - `"draft"` — speculative-decoding draft (v2.1.11+)
 * - `"thinking"` — future thinking-model slot (gh#25)
 *
 * @version 2.1.11
 */
class SecondaryModelLoader {
public:
    /**
     * @brief Lazily load and activate a model for a role.
     *
     * If the role is already loaded against the same config path, this
     * is a no-op (idempotent). If the role is loaded against a different
     * path, the existing backend is unloaded first.
     *
     * @param role Role name (e.g. `"router"`, `"draft"`).
     * @param config ModelConfig for the secondary model.
     * @return true on successful activation, false on failure.
     * @version 2.1.11
     */
    bool ensure_loaded(const std::string& role, const ModelConfig& config);

    /**
     * @brief Get the backend for a role.
     *
     * @param role Role name.
     * @return Backend pointer if loaded, nullptr otherwise.
     * @utility
     * @version 2.1.11
     */
    InferenceBackend* get(const std::string& role) const;

    /**
     * @brief Get the backend for a role as a shared_ptr.
     *
     * Used when callers need to extend backend lifetime beyond the
     * loader (e.g. holding a reference for the duration of a long
     * generation while the loader could otherwise be released).
     *
     * @param role Role name.
     * @return Backend shared_ptr (empty if not loaded).
     * @utility
     * @version 2.1.11
     */
    std::shared_ptr<InferenceBackend> get_shared(
        const std::string& role) const;

    /**
     * @brief Unload and drop a role.
     *
     * @param role Role name.
     * @return true if a role was unloaded, false if it was not loaded.
     * @version 2.1.11
     */
    bool release_role(const std::string& role);

    /**
     * @brief Check whether a role is currently loaded and active.
     *
     * @param role Role name.
     * @return true if `get(role) != nullptr` and the backend reports
     *         it is loaded (state != COLD).
     * @utility
     * @version 2.1.11
     */
    bool is_loaded(const std::string& role) const;

    /**
     * @brief Names of all roles with a currently-loaded backend.
     *
     * @return Sorted list of role names whose backend is non-COLD.
     * @utility
     * @version 2.1.11
     */
    std::vector<std::string> loaded_roles() const;

    /**
     * @brief Fanout: clear prompt/KV cache on every loaded backend.
     *
     * Used by ModelOrchestrator::clear_all_prompt_caches() so the
     * router and draft caches invalidate alongside the main pool when
     * identity content changes (P1-7, v2.0.6-rc16 contract).
     *
     * @utility
     * @version 2.1.11
     */
    void clear_all_prompt_caches();

    /**
     * @brief Unload every role.
     *
     * Mirrors ModelOrchestrator::shutdown() — called during engine
     * teardown. Safe to call repeatedly.
     *
     * @version 2.1.11
     */
    void shutdown();

private:
    /// Mutex guarding the slots_ map. Generation calls on backends
    /// returned by get() do NOT hold this lock — same contract as
    /// the main-tier model_pool_ on ModelOrchestrator.
    mutable std::mutex slots_mutex_;

    /// Role → backend handle. Slots are removed by release_role()
    /// rather than left in a null state, so loaded_roles() iteration
    /// reflects reality without is_loaded() per-slot checks.
    std::unordered_map<std::string, std::shared_ptr<InferenceBackend>>
        slots_;

    /// Per-role recorded path used to detect "same role, same config"
    /// reloads — `ensure_loaded` is idempotent against the same path.
    std::unordered_map<std::string, std::string> slot_paths_;
};

} // namespace entropic
