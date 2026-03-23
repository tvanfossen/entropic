/**
 * @file hooks.h
 * @brief Hook point enum and callback types for engine lifecycle hooks.
 *
 * Defined in v1.8.9 for forward compatibility. Hook implementations
 * ship in v1.9.1. Until then, entropic_register_hook() returns
 * ENTROPIC_ERROR_NOT_IMPLEMENTED for all inputs.
 *
 * @version 1.8.9
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Hook points in the engine lifecycle.
 *
 * Each value identifies a stage where a registered callback is invoked.
 * Pre-hooks (PRE_*) can cancel the operation by returning non-zero.
 * Post-hooks are informational only — return value is ignored.
 *
 * @version 1.8.9
 */
typedef enum {
    ENTROPIC_HOOK_PRE_GENERATE,       ///< Before inference generate call
    ENTROPIC_HOOK_POST_GENERATE,      ///< After inference generate returns
    ENTROPIC_HOOK_ON_STREAM_TOKEN,    ///< Each streaming token emitted
    ENTROPIC_HOOK_PRE_TOOL_CALL,      ///< Before tool execution
    ENTROPIC_HOOK_POST_TOOL_CALL,     ///< After tool execution returns
    ENTROPIC_HOOK_ON_LOOP_ITERATION,  ///< Each agentic loop iteration
    ENTROPIC_HOOK_ON_STATE_CHANGE,    ///< Engine state machine transition
    ENTROPIC_HOOK_ON_ERROR,           ///< Async error occurred
    ENTROPIC_HOOK_ON_DELEGATE,        ///< Delegation to child tier started
    ENTROPIC_HOOK_ON_DELEGATE_COMPLETE, ///< Child delegation completed
    ENTROPIC_HOOK_ON_CONTEXT_ASSEMBLE,  ///< Context window assembled
    ENTROPIC_HOOK_ON_PRE_COMPACT,     ///< Before context compaction
    ENTROPIC_HOOK_ON_POST_COMPACT,    ///< After context compaction
    ENTROPIC_HOOK_ON_MODEL_LOAD,      ///< Model loaded into backend
    ENTROPIC_HOOK_ON_MODEL_UNLOAD,    ///< Model unloaded from backend
    ENTROPIC_HOOK_ON_PERMISSION_CHECK, ///< Permission check evaluated
} entropic_hook_point_t;

/**
 * @brief Hook callback function type.
 *
 * Registered via entropic_register_hook(). Invoked synchronously on the
 * engine thread when the hook point fires.
 *
 * @param hook_point Which hook point fired.
 * @param context_json JSON string with hook-specific context data.
 *        Valid only for the duration of the callback. Copy if needed.
 * @param user_data Opaque pointer from registration.
 * @return 0 to continue, non-zero to cancel the operation (pre-hooks only).
 *         Return value is ignored for post-hooks and informational hooks.
 *
 * @callback
 * @version 1.8.9
 */
typedef int (*entropic_hook_callback_t)(
    entropic_hook_point_t hook_point,
    const char* context_json,
    void* user_data);

#ifdef __cplusplus
}
#endif
