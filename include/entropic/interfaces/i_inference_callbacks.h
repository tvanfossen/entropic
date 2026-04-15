// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file i_inference_callbacks.h
 * @brief Function pointer types for core-to-inference communication.
 *
 * Core does not #include any inference headers. These function pointer
 * types define the contract. The facade (librentropic.so) wires them
 * to the actual InferenceBackend at engine creation time.
 *
 * @par Memory ownership
 * - result_json outputs are allocated by the callee. Caller must free
 *   with the callee's free function (e.g., entropic_inference_free).
 * - Input strings (messages_json, params_json, prompt) are borrowed
 *   for the duration of the call only.
 *
 * @version 1.8.4
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generate a response (batch mode).
 * @param messages_json JSON array of messages.
 * @param params_json Generation parameters JSON.
 * @param[out] result_json Output: JSON result string. Caller must free.
 * @param user_data Opaque pointer to backend instance.
 * @return 0 on success, error code on failure.
 * @callback
 * @version 1.8.4
 */
typedef int (*entropic_generate_fn)(
    const char* messages_json,
    const char* params_json,
    char** result_json,
    void* user_data);

/**
 * @brief Generate a response with streaming.
 * @param messages_json JSON array of messages.
 * @param params_json Generation parameters JSON.
 * @param on_token Callback invoked per token.
 * @param token_user_data Opaque pointer for on_token callback.
 * @param cancel Pointer to atomic cancel flag (1 = cancel).
 * @param user_data Opaque pointer to backend instance.
 * @return 0 on success, error code on failure.
 * @callback
 * @version 1.8.4
 */
typedef int (*entropic_generate_streaming_fn)(
    const char* messages_json,
    const char* params_json,
    void (*on_token)(const char* token, size_t len, void* user_data),
    void* token_user_data,
    int* cancel,
    void* user_data);

/**
 * @brief Route messages to determine tier.
 * @param messages_json JSON array of messages.
 * @param[out] result_json Output: JSON routing result. Caller must free.
 * @param user_data Opaque pointer to orchestrator.
 * @return 0 on success.
 * @callback
 * @version 1.8.4
 */
typedef int (*entropic_route_fn)(
    const char* messages_json,
    char** result_json,
    void* user_data);

/**
 * @brief Raw text completion without chat template.
 * @param prompt Raw prompt string (no chat formatting).
 * @param params_json Generation parameters JSON.
 * @param[out] result_json Output: JSON result string. Caller must free.
 * @param user_data Opaque pointer to backend instance.
 * @return 0 on success, error code on failure.
 * @callback
 * @version 1.8.4
 */
typedef int (*entropic_complete_fn)(
    const char* prompt,
    const char* params_json,
    char** result_json,
    void* user_data);

/**
 * @brief Parse tool calls from raw model output.
 * @param raw_content Raw model output string.
 * @param[out] cleaned_content Output: cleaned content. Caller must free.
 * @param[out] tool_calls_json Output: JSON array of tool calls. Caller must free.
 * @param user_data Opaque pointer to adapter.
 * @return 0 on success.
 * @callback
 * @version 1.8.4
 */
typedef int (*entropic_parse_tool_calls_fn)(
    const char* raw_content,
    char** cleaned_content,
    char** tool_calls_json,
    void* user_data);

/**
 * @brief Check if a response is complete (no pending work).
 * @param content Response content string.
 * @param tool_calls_json JSON array of tool calls (may be "[]").
 * @param user_data Opaque pointer to adapter.
 * @return 1 if complete, 0 if not.
 * @callback
 * @version 1.8.4
 */
typedef int (*entropic_is_response_complete_fn)(
    const char* content,
    const char* tool_calls_json,
    void* user_data);

/**
 * @brief Free a string allocated by the inference layer.
 * @param ptr Pointer to free. NULL is a safe no-op.
 * @callback
 * @version 1.8.4
 */
typedef void (*entropic_inference_free_fn)(void* ptr);

/**
 * @brief Get formatted tool prompt for a tier.
 *
 * Returns the adapter-formatted tool definitions filtered by the
 * tier's allowed_tools. The caller must free the result with the
 * interface's free_fn.
 *
 * @param tier Tier name (e.g., "lead", "researcher").
 * @param[out] result Output: formatted tool prompt string. Caller frees.
 * @param user_data Opaque pointer (facade context).
 * @return 0 on success, non-zero if tier has no tools.
 * @callback
 * @version 2.0.4
 */
typedef int (*entropic_get_tool_prompt_fn)(
    const char* tier,
    char** result,
    void* user_data);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

namespace entropic {

/**
 * @brief Inference interface contract injected into AgentEngine.
 *
 * Defined as a contract header — the engine depends on this interface,
 * not on the inference library. The facade wires concrete implementations
 * at engine creation time.
 *
 * @version 1.8.4
 */
struct InferenceInterface {
    entropic_generate_fn generate = nullptr;                 ///< Batch generation
    entropic_generate_streaming_fn generate_stream = nullptr; ///< Streaming generation
    entropic_complete_fn complete = nullptr;                  ///< Raw text completion
    entropic_route_fn route = nullptr;                       ///< Tier routing
    entropic_parse_tool_calls_fn parse_tool_calls = nullptr; ///< Tool call parsing
    entropic_is_response_complete_fn is_response_complete = nullptr; ///< Completion check
    entropic_inference_free_fn free_fn = nullptr;            ///< Free allocated strings
    entropic_get_tool_prompt_fn get_tool_prompt = nullptr;   ///< Tool definitions for tier (v2.0.4)
    void* backend_data = nullptr;                            ///< Opaque backend pointer
    void* orchestrator_data = nullptr;                       ///< Opaque orchestrator pointer
    void* adapter_data = nullptr;                            ///< Opaque adapter pointer
    void* tool_prompt_data = nullptr;                        ///< Opaque pointer for get_tool_prompt (v2.0.4)
};

} // namespace entropic

#endif
