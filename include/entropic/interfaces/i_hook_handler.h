// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file i_hook_handler.h
 * @brief Hook dispatch interface injected into engine subsystems.
 *
 * Subsystems call through this interface to fire hooks without
 * depending on HookRegistry or librentropic-core.so internals.
 * The facade wires function pointers to the HookRegistry owned
 * by the engine handle.
 *
 * @version 1.9.1
 */

#pragma once

#include <entropic/types/hooks.h>

#ifdef __cplusplus

namespace entropic {

/**
 * @brief Hook dispatch interface injected into subsystems.
 *
 * Function pointers + opaque registry pointer. Same pattern as
 * InferenceInterface (v1.8.4) — no compile-time dependency between
 * .so libraries.
 *
 * @par Usage
 * @code
 *   char* modified = nullptr;
 *   int rc = hooks.fire_pre(hooks.registry,
 *       ENTROPIC_HOOK_PRE_GENERATE, context_json, &modified);
 *   if (rc != 0) { return; } // cancelled
 * @endcode
 *
 * @version 1.9.1
 */
struct HookInterface {
    /**
     * @brief Fire pre-hooks. Returns modified context or cancellation.
     * @param registry Opaque HookRegistry pointer.
     * @param point Hook point.
     * @param context_json Input JSON.
     * @param out_json Output: modified JSON or NULL.
     * @return 0 = proceed, non-zero = cancelled.
     * @version 1.9.1
     */
    int (*fire_pre)(void* registry, entropic_hook_point_t point,
                    const char* context_json, char** out_json) = nullptr;

    /**
     * @brief Fire post-hooks. Returns transformed result.
     * @param registry Opaque HookRegistry pointer.
     * @param point Hook point.
     * @param context_json Input JSON.
     * @param out_json Output: transformed JSON or NULL.
     * @version 1.9.1
     */
    void (*fire_post)(void* registry, entropic_hook_point_t point,
                      const char* context_json, char** out_json) = nullptr;

    /**
     * @brief Fire informational hooks (no modify, no cancel).
     * @param registry Opaque HookRegistry pointer.
     * @param point Hook point.
     * @param context_json Context JSON.
     * @version 1.9.1
     */
    void (*fire_info)(void* registry, entropic_hook_point_t point,
                      const char* context_json) = nullptr;

    void* registry = nullptr; ///< Opaque pointer to HookRegistry
};

} // namespace entropic

#endif
