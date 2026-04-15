// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file hook_registry.cpp
 * @brief HookRegistry implementation — registration, dispatch, error handling.
 * @version 1.9.1
 */

#include <entropic/core/hook_registry.h>
#include <entropic/types/logging.h>

#include <algorithm>
#include <cstdlib>
#include <exception>

static auto logger = entropic::log::get("core.hook_registry");

namespace entropic {

/**
 * @brief Check if a hook point value is valid.
 * @param point Hook point to validate.
 * @return true if 0 <= point < ENTROPIC_HOOK_COUNT_.
 * @utility
 * @version 1.9.1
 */
bool HookRegistry::is_valid_point(entropic_hook_point_t point) {
    return point >= 0
        && point < ENTROPIC_HOOK_COUNT_;
}

/**
 * @brief Register a hook callback at a hook point.
 * @param point Hook point.
 * @param callback Function pointer.
 * @param user_data Opaque pointer.
 * @param priority Execution order (ascending).
 * @return ENTROPIC_OK or ENTROPIC_ERROR_INVALID_CONFIG.
 * @internal
 * @version 1.9.1
 */
entropic_error_t HookRegistry::register_hook(
    entropic_hook_point_t point,
    entropic_hook_callback_t callback,
    void* user_data,
    int priority) {
    if (!is_valid_point(point)) {
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }

    auto idx = static_cast<size_t>(point);
    std::unique_lock lock(mutexes_[idx]);

    HookEntry entry{callback, user_data, priority};
    auto& vec = entries_[idx];

    // Insert sorted by priority (ascending)
    auto pos = std::lower_bound(
        vec.begin(), vec.end(), entry,
        [](const HookEntry& a, const HookEntry& b) {
            return a.priority < b.priority;
        });
    vec.insert(pos, entry);

    logger->info("Registered hook: point={} priority={} total={}",
                 static_cast<int>(point), priority, vec.size());
    return ENTROPIC_OK;
}

/**
 * @brief Deregister a hook callback.
 * @param point Hook point.
 * @param callback Callback to match.
 * @param user_data user_data to match.
 * @return ENTROPIC_OK (idempotent).
 * @internal
 * @version 1.9.1
 */
entropic_error_t HookRegistry::deregister_hook(
    entropic_hook_point_t point,
    entropic_hook_callback_t callback,
    void* user_data) {
    if (!is_valid_point(point)) {
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }

    auto idx = static_cast<size_t>(point);
    std::unique_lock lock(mutexes_[idx]);

    auto& vec = entries_[idx];
    auto it = std::find_if(vec.begin(), vec.end(),
        [callback, user_data](const HookEntry& e) {
            return e.callback == callback
                && e.user_data == user_data;
        });

    if (it != vec.end()) {
        vec.erase(it);
        logger->info("Deregistered hook: point={}", static_cast<int>(point));
    }
    return ENTROPIC_OK;
}

/**
 * @brief Take a snapshot of entries for dispatch.
 * @param point Hook point.
 * @return Copy of the entry vector (already sorted).
 * @internal
 * @version 1.9.1
 */
std::vector<HookEntry> HookRegistry::snapshot(
    entropic_hook_point_t point) const {
    auto idx = static_cast<size_t>(point);
    std::shared_lock lock(mutexes_[idx]);
    return entries_[idx];
}

/**
 * @brief Fire pre-hooks with modification chaining and cancellation.
 * @param point Hook point.
 * @param context_json Input context JSON.
 * @param out_json Output: accumulated modified JSON, or NULL.
 * @return 0 = proceed, non-zero = cancelled.
 * @internal
 * @version 2.0.0
 */
int HookRegistry::fire_pre(
    entropic_hook_point_t point,
    const char* context_json,
    char** out_json) {
    *out_json = nullptr;

    if (!is_valid_point(point)) {
        return 0;
    }

    auto entries = snapshot(point);
    logger->info("fire_pre: point={}, {} handler(s)",
                 static_cast<int>(point), entries.size());
    char* accumulated = nullptr;
    int result = 0;

    for (const auto& entry : entries) {
        const char* input = accumulated ? accumulated : context_json;
        char* modified = nullptr;
        int rc = 0;

        try {
            rc = entry.callback(point, input, &modified, entry.user_data);
        } catch (const std::exception& e) {
            logger->warn("Pre-hook threw for point={}: {}",
                         static_cast<int>(point), e.what());
            continue;
        } catch (...) {
            logger->warn("Pre-hook threw unknown exception for point={}",
                         static_cast<int>(point));
            continue;
        }

        if (rc != 0) {
            free(accumulated);
            free(modified);
            accumulated = nullptr;
            result = rc;
            break;
        }

        if (modified != nullptr) {
            free(accumulated);
            accumulated = modified;
        }
    }

    *out_json = accumulated;
    return result;
}

/**
 * @brief Fire post-hooks with result transformation chaining.
 * @param point Hook point.
 * @param context_json Input result JSON.
 * @param out_json Output: accumulated transformed JSON, or NULL.
 * @internal
 * @version 1.9.1
 */
void HookRegistry::fire_post(
    entropic_hook_point_t point,
    const char* context_json,
    char** out_json) {
    *out_json = nullptr;

    if (!is_valid_point(point)) {
        return;
    }

    auto entries = snapshot(point);
    if (entries.empty()) {
        return;
    }

    char* accumulated = nullptr;

    for (const auto& entry : entries) {
        const char* input = accumulated ? accumulated : context_json;
        char* modified = nullptr;

        try {
            entry.callback(point, input, &modified, entry.user_data);
        } catch (const std::exception& e) {
            logger->warn("Post-hook threw for point={}: {}",
                         static_cast<int>(point), e.what());
            continue;
        } catch (...) {
            logger->warn("Post-hook threw unknown exception for point={}",
                         static_cast<int>(point));
            continue;
        }

        if (modified != nullptr) {
            free(accumulated);
            accumulated = modified;
        }
    }

    *out_json = accumulated;
}

/**
 * @brief Fire informational hooks (no modify, no cancel).
 * @param point Hook point.
 * @param context_json Context JSON.
 * @internal
 * @version 1.9.1
 */
void HookRegistry::fire_info(
    entropic_hook_point_t point,
    const char* context_json) {
    if (!is_valid_point(point)) {
        return;
    }

    auto entries = snapshot(point);

    for (const auto& entry : entries) {
        char* modified = nullptr;
        try {
            entry.callback(point, context_json, &modified, entry.user_data);
        } catch (const std::exception& e) {
            logger->warn("Info hook threw for point={}: {}",
                         static_cast<int>(point), e.what());
        } catch (...) {
            logger->warn("Info hook threw unknown exception for point={}",
                         static_cast<int>(point));
        }
        // Ignore return value and modified_json for info hooks
        free(modified);
    }
}

/**
 * @brief Get the number of registered hooks for a point.
 * @param point Hook point.
 * @return Entry count.
 * @internal
 * @version 1.9.1
 */
size_t HookRegistry::hook_count(entropic_hook_point_t point) const {
    if (!is_valid_point(point)) {
        return 0;
    }
    auto idx = static_cast<size_t>(point);
    std::shared_lock lock(mutexes_[idx]);
    return entries_[idx].size();
}

} // namespace entropic
