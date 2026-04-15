// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file compactor_registry.h
 * @brief Per-identity compactor registration and dispatch.
 *
 * Manages custom compactor functions that replace or augment the
 * default value-density compaction strategy. Consumers register
 * C function pointers per identity; the registry resolves which
 * compactor to call based on identity → global → built-in default.
 *
 * @par Thread safety
 * Registration/deregistration take a write lock. Dispatch takes a
 * read lock to snapshot the compactor, then releases before calling.
 * Safe to register from one thread while compacting on another.
 *
 * @par Ownership
 * Owned by the engine handle. One CompactorRegistry per engine instance.
 * The default compactor (value-density) is set at construction.
 *
 * @version 1.9.9
 */

#pragma once

#include <entropic/core/compaction.h>
#include <entropic/entropic.h>

#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace entropic {

/**
 * @brief Internal C++ compactor function type.
 *
 * Takes messages + config + identity, returns CompactionResult.
 * Wraps C function pointers with C++ types for internal use.
 *
 * @version 1.9.9
 */
using CompactorFn = std::function<CompactionResult(
    const std::vector<Message>& messages,
    const CompactionConfig& config,
    const std::string& identity)>;

/**
 * @brief A registered compactor entry.
 * @version 1.9.9
 */
struct CompactorEntry {
    entropic_compactor_fn c_callback = nullptr; ///< C function pointer (NULL for C++ compactors)
    void* user_data = nullptr;                  ///< Opaque data for C callback
    CompactorFn cpp_fn;                         ///< C++ function (wraps c_callback or native)
};

/**
 * @brief Per-identity compactor registry and dispatch.
 *
 * Resolution order when compaction triggers:
 * 1. Per-identity compactor (if registered for the exact identity)
 * 2. Global custom compactor (identity="", if registered)
 * 3. Built-in default (v1.8.4 value-density via CompactionManager)
 *
 * Custom compactor failure is non-fatal: falls back to default with
 * a WARNING log. Only if both custom AND default fail does the
 * registry return ENTROPIC_ERROR_COMPACTION_FAILED.
 *
 * @version 1.9.9
 */
class CompactorRegistry {
public:
    /**
     * @brief Construct with default compactor.
     * @param default_manager Reference to v1.8.4 CompactionManager.
     * @version 1.9.9
     */
    explicit CompactorRegistry(CompactionManager& default_manager);

    /**
     * @brief Register a compactor for a specific identity.
     * @param identity Identity name ("" for global fallback).
     * @param compactor C function pointer.
     * @param user_data Opaque pointer passed to compactor.
     * @return ENTROPIC_OK on success.
     *         ENTROPIC_ERROR_INVALID_CONFIG if compactor is NULL.
     *
     * Replaces any previously registered compactor for this identity.
     *
     * @threadsafety Write-locks the registry.
     * @version 1.9.9
     */
    entropic_error_t register_compactor(
        const std::string& identity,
        entropic_compactor_fn compactor,
        void* user_data);

    /**
     * @brief Deregister a compactor for a specific identity.
     * @param identity Identity name ("" for global fallback).
     * @return ENTROPIC_OK on success (idempotent).
     *
     * After deregistration, the identity falls back to the global
     * custom compactor (if any), then to the built-in default.
     *
     * @threadsafety Write-locks the registry.
     * @version 1.9.9
     */
    entropic_error_t deregister_compactor(
        const std::string& identity);

    /**
     * @brief Run compaction using the appropriate compactor.
     * @param identity Current identity name.
     * @param messages Messages to compact.
     * @param config Compaction configuration.
     * @return CompactionResult with compacted messages and metadata.
     *
     * Resolution: per-identity → global custom → built-in default.
     * On custom compactor failure, falls back to default with WARNING.
     *
     * @threadsafety Read-locks to snapshot, releases before calling.
     * @version 1.9.9
     */
    CompactionResult compact(
        const std::string& identity,
        const std::vector<Message>& messages,
        const CompactionConfig& config);

    /**
     * @brief Check if a custom compactor is registered for an identity.
     * @param identity Identity name.
     * @return true if a non-default compactor will be used.
     *
     * @threadsafety Read-locks the registry.
     * @version 1.9.9
     */
    bool has_custom_compactor(const std::string& identity) const;

    /**
     * @brief Get the default CompactionManager reference.
     * @return Reference to the v1.8.4 CompactionManager.
     * @utility
     * @version 1.9.9
     */
    CompactionManager& default_manager() { return default_manager_; }

private:
    /**
     * @brief Run the built-in default compactor.
     * @param identity Identity name for result metadata.
     * @param messages Messages to compact.
     * @param config Compaction configuration.
     * @return CompactionResult from default strategy.
     * @version 1.9.9
     */
    CompactionResult run_default(
        const std::string& identity,
        const std::vector<Message>& messages,
        const CompactionConfig& config);

    /**
     * @brief Run a custom compactor with fallback to default.
     * @param selected Custom compactor function.
     * @param source Compactor source label.
     * @param identity Identity name.
     * @param messages Messages to compact.
     * @param config Compaction configuration.
     * @return CompactionResult from custom or fallback default.
     * @version 1.9.9
     */
    CompactionResult run_custom(
        const CompactorFn& selected,
        const std::string& source,
        const std::string& identity,
        const std::vector<Message>& messages,
        const CompactionConfig& config);

    /**
     * @brief Wrap a C function pointer into a CompactorFn.
     * @param compactor C function pointer.
     * @param user_data Opaque pointer.
     * @return C++ callable wrapping the C callback.
     * @version 1.9.9
     */
    static CompactorFn wrap_c_compactor(
        entropic_compactor_fn compactor,
        void* user_data);

    CompactionManager& default_manager_;                        ///< v1.8.4 default
    CompactorFn default_compactor_;                             ///< Built-in default fn
    std::unordered_map<std::string, CompactorEntry> compactors_; ///< Per-identity map
    mutable std::shared_mutex mutex_;                            ///< Guards compactors_
};

} // namespace entropic
