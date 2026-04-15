// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file hook_registry.h
 * @brief Thread-safe hook registration and dispatch.
 *
 * HookRegistry manages per-hook-point callback lists with priority
 * ordering. Registration/deregistration take write locks. Dispatch
 * takes a read lock and copies entries to a snapshot, so callbacks
 * can safely register/deregister other hooks without deadlock.
 *
 * Lives in librentropic-core.so. Not exposed across .so boundaries —
 * the C API in entropic.h is the only external interface.
 *
 * @version 1.9.1
 */

#pragma once

#include <entropic/types/hooks.h>
#include <entropic/types/error.h>

#include <array>
#include <shared_mutex>
#include <vector>

namespace entropic {

/**
 * @brief A single registered hook entry.
 * @version 1.9.1
 */
struct HookEntry {
    entropic_hook_callback_t callback; ///< Function pointer
    void* user_data;                   ///< Opaque consumer data
    int priority;                      ///< Execution order (ascending)
};

/**
 * @brief Thread-safe hook registration and dispatch.
 *
 * Owns a vector of HookEntry per hook point. Registration and
 * deregistration take a write lock. Dispatch takes a read lock
 * and a snapshot of the entry list (so hooks can register/deregister
 * other hooks from within a callback without deadlock).
 *
 * @version 1.9.1
 */
class HookRegistry {
public:
    /**
     * @brief Register a hook callback at a hook point.
     * @param point Hook point enum value.
     * @param callback Function pointer.
     * @param user_data Opaque consumer data.
     * @param priority Execution order (0 = first, ascending).
     * @return ENTROPIC_OK or ENTROPIC_ERROR_INVALID_CONFIG.
     * @threadsafety Write-locks the hook point's mutex.
     * @version 1.9.1
     */
    entropic_error_t register_hook(
        entropic_hook_point_t point,
        entropic_hook_callback_t callback,
        void* user_data,
        int priority);

    /**
     * @brief Deregister a hook callback.
     *
     * Matches on (point, callback, user_data) triple. Idempotent —
     * returns ENTROPIC_OK if no match found.
     *
     * @param point Hook point.
     * @param callback Callback to remove.
     * @param user_data user_data from registration.
     * @return ENTROPIC_OK or ENTROPIC_ERROR_INVALID_CONFIG.
     * @threadsafety Write-locks the hook point's mutex.
     * @version 1.9.1
     */
    entropic_error_t deregister_hook(
        entropic_hook_point_t point,
        entropic_hook_callback_t callback,
        void* user_data);

    /**
     * @brief Fire pre-hooks. Returns modified context or cancellation.
     * @param point The hook point.
     * @param context_json Input context JSON.
     * @param out_json Output: modified JSON (caller frees), or NULL.
     * @return 0 = proceed, non-zero = cancelled.
     * @threadsafety Read-locks, then dispatches on snapshot.
     * @version 1.9.1
     */
    int fire_pre(
        entropic_hook_point_t point,
        const char* context_json,
        char** out_json);

    /**
     * @brief Fire post-hooks. Returns transformed result.
     * @param point The hook point.
     * @param context_json Input result JSON.
     * @param out_json Output: transformed JSON (caller frees), or NULL.
     * @threadsafety Read-locks, then dispatches on snapshot.
     * @version 1.9.1
     */
    void fire_post(
        entropic_hook_point_t point,
        const char* context_json,
        char** out_json);

    /**
     * @brief Fire informational hooks (no modify, no cancel).
     * @param point The hook point.
     * @param context_json Context JSON.
     * @threadsafety Read-locks, then dispatches on snapshot.
     * @version 1.9.1
     */
    void fire_info(
        entropic_hook_point_t point,
        const char* context_json);

    /**
     * @brief Get the number of registered hooks for a point.
     * @param point Hook point.
     * @return Entry count, or 0 if invalid point.
     * @threadsafety Read-locks the hook point's mutex.
     * @version 1.9.1
     */
    size_t hook_count(entropic_hook_point_t point) const;

private:
    /**
     * @brief Check if a hook point value is valid.
     * @param point Hook point to validate.
     * @return true if within range.
     * @utility
     * @version 1.9.1
     */
    static bool is_valid_point(entropic_hook_point_t point);

    /**
     * @brief Take a priority-sorted snapshot of entries for a point.
     * @param point Hook point.
     * @return Copy of entry vector.
     * @internal
     * @version 1.9.1
     */
    std::vector<HookEntry> snapshot(entropic_hook_point_t point) const;

    /// Per-hook-point entry list, sorted by priority.
    std::array<std::vector<HookEntry>, ENTROPIC_HOOK_COUNT_> entries_;

    /// Read-write lock per hook point.
    mutable std::array<std::shared_mutex, ENTROPIC_HOOK_COUNT_> mutexes_;
};

} // namespace entropic
