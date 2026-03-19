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
 * @version 1.8.2
 */

#pragma once

#include <entropic/inference/backend.h>

#include <llama.h>

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
 * @version 1.8.2
 */
class LlamaCppBackend : public InferenceBackend {
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

    /* ── llama.cpp handles ───────────────────────────────── */

    llama_model* model_ = nullptr;             ///< Loaded model (WARM+)
    llama_context* ctx_ = nullptr;             ///< Inference context (ACTIVE)
    const llama_vocab* vocab_ = nullptr;       ///< Vocabulary (from model_)

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
};

} // namespace entropic
