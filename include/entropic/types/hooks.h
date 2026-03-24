/**
 * @file hooks.h
 * @brief Hook point enum and callback types for engine lifecycle hooks.
 *
 * Consumers register callbacks at hook points to intercept, modify,
 * inspect, and transform engine behavior without forking. Pre-hooks
 * can modify context or cancel operations. Post-hooks can transform
 * results. Failing hooks are logged and skipped — never crash the engine.
 *
 * @version 1.9.1
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Hook points in the engine lifecycle.
 *
 * Each value identifies a stage where registered callbacks are invoked.
 * Pre-hooks can cancel the operation by returning non-zero.
 * Post-hooks can transform results via the modified_json out-param.
 * Informational hooks fire for observation only.
 *
 * New values are appended only — no renumbering (ABI stable).
 *
 * @version 1.9.1
 */
typedef enum {
    /* --- v1.8.9 originals (unchanged) --- */
    ENTROPIC_HOOK_PRE_GENERATE,         ///<  0: Before inference generate call
    ENTROPIC_HOOK_POST_GENERATE,        ///<  1: After inference generate returns
    ENTROPIC_HOOK_ON_STREAM_TOKEN,      ///<  2: Each streaming token emitted
    ENTROPIC_HOOK_PRE_TOOL_CALL,        ///<  3: Before tool execution
    ENTROPIC_HOOK_POST_TOOL_CALL,       ///<  4: After tool execution returns
    ENTROPIC_HOOK_ON_LOOP_ITERATION,    ///<  5: Each agentic loop iteration
    ENTROPIC_HOOK_ON_STATE_CHANGE,      ///<  6: Engine state machine transition
    ENTROPIC_HOOK_ON_ERROR,             ///<  7: Async error occurred
    ENTROPIC_HOOK_ON_DELEGATE,          ///<  8: Delegation to child tier started
    ENTROPIC_HOOK_ON_DELEGATE_COMPLETE, ///<  9: Child delegation completed
    ENTROPIC_HOOK_ON_CONTEXT_ASSEMBLE,  ///< 10: Context window assembled
    ENTROPIC_HOOK_ON_PRE_COMPACT,       ///< 11: Before context compaction
    ENTROPIC_HOOK_ON_POST_COMPACT,      ///< 12: After context compaction
    ENTROPIC_HOOK_ON_MODEL_LOAD,        ///< 13: Model loaded into backend
    ENTROPIC_HOOK_ON_MODEL_UNLOAD,      ///< 14: Model unloaded from backend
    ENTROPIC_HOOK_ON_PERMISSION_CHECK,  ///< 15: Permission check evaluated

    /* --- v1.9.1 additions --- */
    ENTROPIC_HOOK_ON_ADAPTER_SWAP,      ///< 16: Adapter/LoRA swap requested
    ENTROPIC_HOOK_ON_VRAM_PRESSURE,     ///< 17: VRAM pressure detected
    ENTROPIC_HOOK_ON_DIRECTIVE,         ///< 18: Before processing a directive
    ENTROPIC_HOOK_ON_CUSTOM_DIRECTIVE,  ///< 19: Unrecognized directive type
    ENTROPIC_HOOK_ON_LOOP_START,        ///< 20: Agentic loop entry
    ENTROPIC_HOOK_ON_LOOP_END,          ///< 21: Agentic loop exit

    ENTROPIC_HOOK_COUNT_                ///< Sentinel — not a valid hook point
} entropic_hook_point_t;

/**
 * @brief Hook callback function type.
 *
 * Pre-hook semantics:
 *   - Return 0 + *modified_json == NULL:  proceed unchanged
 *   - Return 0 + *modified_json != NULL:  proceed with modified context
 *   - Return non-zero:                    cancel the operation
 *
 * Post-hook semantics:
 *   - Return value ignored (post-hooks cannot cancel)
 *   - *modified_json != NULL:  transform the result
 *   - *modified_json == NULL:  no transformation
 *
 * The engine frees *modified_json after consuming it. The callback
 * MUST allocate it with entropic_alloc() (not malloc, not new, not stack).
 *
 * @param hook_point     Which hook point fired.
 * @param context_json   JSON string with hook-specific context data.
 *                       Owned by the engine, valid only for this call.
 * @param modified_json  Output: replacement JSON, or NULL for no change.
 *                       Allocated by callback with entropic_alloc(),
 *                       freed by engine.
 * @param user_data      Opaque pointer from registration.
 * @return 0 to continue, non-zero to cancel (pre-hooks only).
 *
 * @callback
 * @version 1.9.1
 */
typedef int (*entropic_hook_callback_t)(
    entropic_hook_point_t hook_point,
    const char* context_json,
    char** modified_json,
    void* user_data);

#ifdef __cplusplus
}
#endif
