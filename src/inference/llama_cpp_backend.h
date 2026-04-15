// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file llama_cpp_backend.h
 * @brief LlamaCppBackend — llama.cpp C API integration.
 *
 * Versioned subclass pattern: LlamaCppBackend provides common llama.cpp
 * patterns (decode loop, sampler chain, tokenization). The pinned-commit
 * subclass (LlamaCppBackend_b8420) overrides API-version-specific calls.
 *
 * @par VRAM lifecycle mapping
 * - COLD: nothing allocated
 * - WARM: llama_model loaded (CPU mmap+mlock, n_gpu_layers=0)
 * - ACTIVE: model reloaded with gpu_layers, llama_context created
 *
 * @par Key differences from Python LlamaCppBackend
 * - Direct llama.cpp C API (not llama-cpp-python wrapper)
 * - No Python GIL — generation runs natively
 * - No asyncio bridge — streaming is synchronous with callback
 *
 * Internal to inference .so — not exposed across boundaries.
 *
 * @version 1.9.13
 */

#pragma once

#include <entropic/inference/backend.h>

#include "prompt_cache.h"

#include <llama.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief LlamaCppBackend — common llama.cpp patterns (15% layer).
 *
 * Provides decode loop, sampler chain creation, tokenization helpers.
 * Pinned-version subclass overrides do_load/do_activate with
 * version-specific API calls.
 *
 * @version 1.8.3
 */
class LlamaCppBackend : public InferenceBackend {
public:
    /**
     * @brief Set prompt cache configuration.
     *
     * Must be called before activate(). The config is consumed when
     * the cache is constructed during do_activate().
     *
     * @param config Prompt cache configuration.
     * @utility
     * @version 1.8.3
     */
    void set_prompt_cache_config(const PromptCacheConfig& config) {
        prompt_cache_config_ = config;
    }

    /**
     * @brief Tokenize text to token IDs using model vocabulary.
     * @param text Input text.
     * @return Token ID vector with BOS.
     * @version 1.10.2
     */
    std::vector<int32_t> tokenize_text(
        const std::string& text) const override;

    /* ── llama.cpp handle accessors (v1.9.2) ────────────── */

    /**
     * @brief Get the loaded llama_model pointer.
     * @return nullptr if state is COLD.
     * @utility
     * @version 1.9.2
     */
    llama_model* llama_model_ptr() { return model_; }

    /**
     * @brief Get the active llama_context pointer.
     * @return nullptr if state is not ACTIVE.
     * @utility
     * @version 1.9.2
     */
    llama_context* llama_context_ptr() { return ctx_; }

protected:
    /* ── Lifecycle overrides ─────────────────────────────── */

    bool do_load(const ModelConfig& config) override;
    bool do_activate() override;
    void do_deactivate() override;
    void do_unload() override;

    /* ── Generation overrides ────────────────────────────── */

    GenerationResult do_generate(
        const std::vector<Message>& messages,
        const GenerationParams& params) override;

    GenerationResult do_generate_streaming(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel) override;

    GenerationResult do_complete(
        const std::string& prompt,
        const GenerationParams& params) override;

    int do_count_tokens(const std::string& text) const override;

    /* ── Evaluation override (v1.9.10) ──────────────────── */

    LogprobResult do_evaluate_logprobs(
        const int32_t* tokens,
        int n_tokens) override;

    /* ── Capability overrides (v1.9.13) ──────────────────── */

    bool do_supports(BackendCapability cap) const override;
    std::string do_backend_name() const override;
    BackendInfo do_info() const override;
    bool do_clear_state(int seq_id) override;

    /* ── llama.cpp handles ───────────────────────────────── */

    llama_model* model_ = nullptr;             ///< Loaded model (WARM+)
    llama_context* ctx_ = nullptr;             ///< Inference context (ACTIVE)
    const llama_vocab* vocab_ = nullptr;       ///< Vocabulary (from model_)

    /* ── Prompt cache ───────────────────────────────────── */

    PromptCacheConfig prompt_cache_config_;      ///< Cache config (v1.8.3)
    std::unique_ptr<PromptCache> prompt_cache_;  ///< KV prefix cache (v1.8.3)

    /* ── Internal helpers ────────────────────────────────── */

    /**
     * @brief Tokenize text using model vocabulary.
     * @param text Input text.
     * @param add_special Add special tokens (BOS).
     * @return Token vector.
     * @version 1.8.2
     */
    std::vector<llama_token> tokenize(
        const std::string& text, bool add_special) const;

    /**
     * @brief Detokenize a single token.
     * @param token Token ID.
     * @return String representation.
     * @version 1.8.2
     */
    std::string detokenize(llama_token token) const;

    /**
     * @brief Apply chat template to messages.
     * @param messages Conversation history.
     * @param params Generation parameters (for enable_thinking).
     * @return Formatted prompt string.
     * @version 1.8.2
     */
    std::string apply_chat_template(
        const std::vector<Message>& messages,
        const GenerationParams& params) const;

    /**
     * @brief Core decode loop — shared by generate and streaming.
     * @param tokens Input token sequence.
     * @param params Generation parameters.
     * @param on_token Per-token callback (nullptr for batch).
     * @param cancel Cancel flag (nullptr for batch).
     * @return GenerationResult.
     * @version 1.8.2
     */
    GenerationResult decode_loop(
        const std::vector<llama_token>& tokens,
        const GenerationParams& params,
        std::function<void(std::string_view)> on_token,
        std::atomic<bool>* cancel);

    /**
     * @brief Run batched prefill on input tokens.
     * @param tokens Input token sequence.
     * @return true on success.
     * @version 1.8.2
     */
    bool run_prefill(const std::vector<llama_token>& tokens);

    /**
     * @brief Generate one token and append to output.
     * @param sampler Sampler chain.
     * @param generated Accumulated output (mutated).
     * @param on_token Streaming callback.
     * @param stop Stop sequences.
     * @return "continue", "stop", "eos", or "error".
     * @version 1.8.2
     */
    std::string step_token(
        llama_sampler* sampler,
        std::string& generated,
        std::function<void(std::string_view)>& on_token,
        const std::vector<std::string>& stop);

    /**
     * @brief Create sampler chain from generation params.
     * @param params Generation parameters.
     * @return Sampler chain (caller frees via llama_sampler_free).
     * @version 1.8.2
     */
    llama_sampler* create_sampler(const GenerationParams& params) const;

    /**
     * @brief Extract the system prompt from messages.
     * @param messages Conversation history.
     * @return System prompt text, empty if no system message.
     * @version 1.8.3
     */
    static std::string extract_system_prompt(
        const std::vector<Message>& messages);

    /**
     * @brief Run prefill with prompt cache integration.
     * @param tokens Full token sequence.
     * @param system_prompt System prompt text for cache key.
     * @param messages Original messages (for prefix boundary).
     * @param params Generation parameters.
     * @return true on success.
     * @version 1.8.3
     */
    bool run_prefill_cached(
        const std::vector<llama_token>& tokens,
        const std::string& system_prompt,
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /**
     * @brief Restore KV state from cache and decode remaining tokens.
     * @param cached Cache entry to restore.
     * @param tokens Full token sequence.
     * @return true on success, false to fall back to full prefill.
     * @version 1.8.3
     */
    bool restore_cached_prefix(
        const CacheEntry* cached,
        const std::vector<llama_token>& tokens);

    /**
     * @brief Save system prefix KV state to cache after full prefill.
     * @param key Cache key.
     * @param prefix_tokens System prefix token count.
     * @version 1.8.3
     */
    void save_prefix_to_cache(const CacheKey& key, int prefix_tokens);

    /**
     * @brief Compute token count of system messages only.
     * @param messages Message list.
     * @param params Generation params (for template).
     * @return Token count, 0 if no system messages.
     * @version 1.8.3
     */
    int compute_prefix_token_count(
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /* ── Evaluation helpers (v1.9.10) ───────────────────── */

    /**
     * @brief Allocate a temporary sequence ID for evaluation.
     * @return Unused seq_id, or -1 if pool is exhausted.
     * @version 1.9.10
     */
    llama_seq_id allocate_temp_seq_id();

    /**
     * @brief Release a temporary sequence ID back to the pool.
     * @param seq_id The seq_id to release.
     * @version 1.9.10
     */
    void release_temp_seq_id(llama_seq_id seq_id);

    /**
     * @brief Extract log-probability for a token from logits.
     *
     * Computes log_softmax(logits)[next_token] using the numerically
     * stable form: logits[t] - max - log(sum(exp(logits - max))).
     *
     * @param logits Raw logits array from llama_get_logits_ith().
     * @param next_token The token to score.
     * @param n_vocab Vocabulary size.
     * @return log P(next_token | context).
     * @version 1.9.10
     */
    static float extract_token_logprob(
        const float* logits,
        int32_t next_token,
        int n_vocab);

    std::mutex seq_id_mutex_;                 ///< Guards temp seq_id pool (v1.9.10)
    std::vector<llama_seq_id> free_seq_ids_;  ///< Available temporary seq_ids (v1.9.10)

    /* ── Architecture detection (v1.9.13) ──────────────── */

    /// @brief True if loaded model is recurrent (GDN/Mamba/RWKV).
    /// Set during do_load() from llama_model_is_recurrent(). Drives
    /// capability reporting (KV_CACHE vs HIDDEN_STATE, speculative
    /// decoding compatibility, etc.).
    /// @version 1.9.13
    bool is_recurrent_ = false;

    /**
     * @brief Check if loaded model is recurrent.
     * @return true if GDN/Mamba/RWKV architecture.
     * @version 1.9.13
     */
    bool is_recurrent() const;
};

} // namespace entropic
