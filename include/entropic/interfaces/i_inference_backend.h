/**
 * @file i_inference_backend.h
 * @brief Pure C interface contract for inference backends.
 *
 * This is the .so boundary for inference. All types are C-safe: opaque
 * handles, error codes, function pointers, const char* JSON strings.
 * No C++ types cross this boundary.
 *
 * @par Memory ownership
 * - Strings returned by generate/complete (via result_json) are allocated
 *   by the backend. Caller must free with entropic_inference_free().
 * - Strings passed IN (messages_json, params_json, prompt) are borrowed
 *   for the duration of the call only.
 * - The on_token callback receives a pointer valid only for the callback
 *   duration. Caller must copy if they need to retain the token.
 *
 * @par Thread safety
 * - State queries (entropic_inference_state) are lock-free.
 * - Lifecycle transitions (load/activate/deactivate/unload) are serialized.
 * - Generation calls require ACTIVE state and do not acquire the
 *   transition lock — concurrent generation is allowed.
 *
 * @version 1.9.13
 */

#pragma once

#include <entropic/types/error.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to an inference backend instance.
 * @version 1.8.2
 */
typedef struct entropic_inference_backend* entropic_inference_backend_t;

/* ── Lifecycle ─────────────────────────────────────────── */

/**
 * @brief Load a model from config (COLD → WARM).
 *
 * @param backend Backend handle.
 * @param config_json JSON string of ModelConfig fields.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_LOAD_FAILED on failure.
 * @version 1.8.2
 */
entropic_error_t entropic_inference_load(
    entropic_inference_backend_t backend,
    const char* config_json);

/**
 * @brief Activate model on GPU (WARM → ACTIVE).
 *
 * If COLD, loads first (convenience path).
 *
 * @param backend Backend handle.
 * @return ENTROPIC_OK on success.
 * @version 1.8.2
 */
entropic_error_t entropic_inference_activate(
    entropic_inference_backend_t backend);

/**
 * @brief Deactivate model (ACTIVE → WARM). No-op if not ACTIVE.
 *
 * @param backend Backend handle.
 * @return ENTROPIC_OK.
 * @version 1.8.2
 */
entropic_error_t entropic_inference_deactivate(
    entropic_inference_backend_t backend);

/**
 * @brief Unload model completely (→ COLD). Releases all RAM + VRAM.
 *
 * @param backend Backend handle.
 * @return ENTROPIC_OK.
 * @version 1.8.2
 */
entropic_error_t entropic_inference_unload(
    entropic_inference_backend_t backend);

/**
 * @brief Query model state (lock-free).
 *
 * @param backend Backend handle.
 * @return State as int: 0=COLD, 1=WARM, 2=ACTIVE.
 *         Maps to entropic_model_state_t values.
 * @version 1.8.2
 */
int entropic_inference_state(entropic_inference_backend_t backend);

/* ── Generation ────────────────────────────────────────── */

/**
 * @brief Generate a response from messages (batch mode).
 *
 * Requires ACTIVE state. Returns ENTROPIC_ERROR_INVALID_STATE otherwise.
 *
 * @param backend Backend handle.
 * @param messages_json JSON array of message objects.
 * @param params_json JSON object of GenerationParams fields.
 * @param[out] result_json Output: JSON result string. Caller frees
 *             with entropic_inference_free().
 * @return ENTROPIC_OK on success.
 * @req REQ-INFER-001
 * @version 1.9.13
 */
entropic_error_t entropic_inference_generate(
    entropic_inference_backend_t backend,
    const char* messages_json,
    const char* params_json,
    char** result_json);

/**
 * @brief Generate with streaming token callback.
 *
 * Requires ACTIVE state. The on_token callback is invoked for each
 * generated token. The token pointer is valid only for the duration
 * of the callback — caller must copy if retention is needed.
 *
 * @param backend Backend handle.
 * @param messages_json JSON array of message objects.
 * @param params_json JSON object of GenerationParams fields.
 * @param on_token Callback for each token. Must not call back into API.
 * @param user_data Opaque pointer forwarded to on_token.
 * @param cancel_flag Pointer to int flag. Caller sets to 1 to cancel.
 *        Checked between tokens — cancellation latency is one token.
 *        May be NULL if cancellation is not needed.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_CANCELLED if cancelled.
 * @req REQ-INFER-003
 * @version 1.9.13
 */
entropic_error_t entropic_inference_generate_streaming(
    entropic_inference_backend_t backend,
    const char* messages_json,
    const char* params_json,
    void (*on_token)(const char* token, size_t len, void* user_data),
    void* user_data,
    int* cancel_flag);

/**
 * @brief Raw text completion without chat template.
 *
 * Used by the router for digit-based classification. The prompt is
 * passed directly to the model without any chat template formatting.
 *
 * @param backend Backend handle.
 * @param prompt Raw prompt string.
 * @param params_json Generation parameters as JSON.
 * @param[out] result_json Output: JSON result string. Caller frees.
 * @return ENTROPIC_OK on success.
 * @req REQ-INFER-004
 * @version 1.9.13
 */
entropic_error_t entropic_inference_complete(
    entropic_inference_backend_t backend,
    const char* prompt,
    const char* params_json,
    char** result_json);

/* ── Utility ───────────────────────────────────────────── */

/**
 * @brief Count tokens in text using model's tokenizer.
 *
 * Returns exact count when model is loaded (WARM or ACTIVE).
 * Returns len/4 estimate when model is COLD.
 *
 * @param backend Backend handle.
 * @param text Text to tokenize.
 * @param text_len Length of text in bytes.
 * @return Token count (exact or estimated).
 * @utility
 * @version 1.9.13
 */
int entropic_inference_count_tokens(
    entropic_inference_backend_t backend,
    const char* text,
    size_t text_len);

/**
 * @brief Destroy backend instance and free all resources.
 *
 * @param backend Backend handle. NULL is a safe no-op.
 * @version 1.8.2
 */
void entropic_inference_destroy(entropic_inference_backend_t backend);

/**
 * @brief Free a string allocated by the inference backend.
 *
 * @param ptr Pointer returned by generate, complete, or similar.
 *        NULL is a safe no-op.
 * @version 1.8.2
 */
void entropic_inference_free(void* ptr);

/* ── Capability + state queries (v1.9.13) ─────────────────── */

/**
 * @brief Query backend capability.
 * @param backend Backend handle.
 * @param capability Capability enum value (see BackendCapability).
 * @return 1 if supported, 0 if not.
 * @version 1.9.13
 */
int entropic_inference_supports(
    entropic_inference_backend_t backend,
    int capability);

/**
 * @brief Get all supported capabilities as bitmask.
 * @param backend Backend handle.
 * @return Bitmask where bit N corresponds to BackendCapability N.
 * @version 1.9.13
 */
uint32_t entropic_inference_capabilities(
    entropic_inference_backend_t backend);

/**
 * @brief Get backend metadata as JSON.
 * @param backend Backend handle.
 * @return JSON string of BackendInfo. Caller must free with
 *         entropic_inference_free().
 * @version 1.9.13
 */
char* entropic_inference_info(
    entropic_inference_backend_t backend);

/**
 * @brief Save model state for a sequence.
 * @param backend Backend handle.
 * @param seq_id Sequence identifier (0 for single-sequence).
 * @param[out] buffer Output: pointer to state data. Caller frees
 *        with entropic_inference_free().
 * @param[out] buffer_size Output: size of state data in bytes.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_NOT_SUPPORTED if
 *         backend has neither KV_CACHE nor HIDDEN_STATE capability.
 * @utility
 * @version 1.9.13
 */
entropic_error_t entropic_inference_save_state(
    entropic_inference_backend_t backend,
    int seq_id,
    void** buffer,
    size_t* buffer_size);

/**
 * @brief Restore model state for a sequence.
 * @param backend Backend handle.
 * @param seq_id Sequence identifier.
 * @param buffer State data from previous save_state call.
 * @param buffer_size Size of state data.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_STATE_INCOMPATIBLE
 *         if buffer is incompatible.
 * @utility
 * @version 1.9.13
 */
entropic_error_t entropic_inference_restore_state(
    entropic_inference_backend_t backend,
    int seq_id,
    const void* buffer,
    size_t buffer_size);

/**
 * @brief Clear/reset model state.
 * @param backend Backend handle.
 * @param seq_id Sequence identifier, or -1 for all sequences.
 * @return ENTROPIC_OK on success.
 * @version 1.9.13
 */
entropic_error_t entropic_inference_clear_state(
    entropic_inference_backend_t backend,
    int seq_id);

/**
 * @brief Generate with explicit sequence ID.
 * @param backend Backend handle.
 * @param seq_id Sequence identifier (0 for single-sequence backends).
 * @param messages_json JSON string of messages.
 * @param params_json JSON string of generation params.
 * @param[out] result_json Output: JSON result string. Caller frees.
 * @return ENTROPIC_OK on success.
 * @utility
 * @version 1.9.13
 */
entropic_error_t entropic_inference_generate_seq(
    entropic_inference_backend_t backend,
    int seq_id,
    const char* messages_json,
    const char* params_json,
    char** result_json);

/**
 * @brief Streaming generation with explicit sequence ID.
 * @param backend Backend handle.
 * @param seq_id Sequence identifier.
 * @param messages_json JSON string of messages.
 * @param params_json JSON string of generation params.
 * @param on_token Callback for each token. Must not call back into API.
 * @param user_data Opaque pointer forwarded to on_token.
 * @param cancel_flag Pointer to int flag. Set to 1 to cancel. May be NULL.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_CANCELLED if cancelled.
 * @utility
 * @version 1.9.13
 */
entropic_error_t entropic_inference_generate_streaming_seq(
    entropic_inference_backend_t backend,
    int seq_id,
    const char* messages_json,
    const char* params_json,
    void (*on_token)(const char* token, size_t len, void* user_data),
    void* user_data,
    int* cancel_flag);

#ifdef __cplusplus
}
#endif

/*
 * Plugin export requirements:
 *
 *   extern "C" ENTROPIC_EXPORT int entropic_plugin_api_version();
 *   extern "C" ENTROPIC_EXPORT entropic_inference_backend_t
 *       entropic_create_inference_backend();
 */
