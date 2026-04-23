// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file backend.h
 * @brief InferenceBackend concrete base class.
 *
 * Owns: lifecycle state machine, transition mutex, metrics, logging.
 * Subclasses override: do_load, do_activate, do_deactivate, do_unload,
 * do_generate, do_generate_streaming, do_complete, do_count_tokens,
 * do_evaluate_logprobs.
 *
 * @par Thread safety
 * - state() is lock-free (std::atomic<ModelState>)
 * - load/activate/deactivate/unload acquire transition_mutex_
 * - generate/generate_streaming/complete require ACTIVE state,
 *   do NOT acquire transition_mutex_ (generation is concurrent with
 *   state queries, but not with state transitions)
 * - evaluate_logprobs acquires eval_mutex_ (separate from transition
 *   and generation — evaluation can run concurrently with generation)
 *
 * @par State machine
 * @code
 *   COLD ──load()──> WARM ──activate()──> ACTIVE
 *     ^               |                     |
 *     └──unload()─────┘<──deactivate()──────┘
 * @endcode
 *
 * Internal to inference .so — not exposed across boundaries.
 *
 * @version 1.9.13
 */

#pragma once

#include <entropic/types/backend_capability.h>
#include <entropic/types/config.h>
#include <entropic/types/generation_result.h>
#include <entropic/types/logprob_result.h>
#include <entropic/types/message.h>
#include <entropic/interfaces/i_hook_handler.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace entropic {

/**
 * @brief Concrete base class for inference backends (80% logic).
 *
 * Public methods implement the lifecycle state machine, transition
 * locking, timing, and logging. Protected virtual methods are the
 * 20% that subclasses override with backend-specific logic.
 *
 * @par Transition rules (enforced by base class)
 * - load():              COLD → WARM (no-op if WARM/ACTIVE)
 * - activate():          WARM → ACTIVE (auto-loads if COLD)
 * - deactivate():        ACTIVE → WARM (no-op if not ACTIVE)
 * - unload():            any → COLD (idempotent)
 * - load_and_activate(): COLD → ACTIVE (convenience)
 *
 * Invalid transitions are no-ops with INFO log (not errors).
 *
 * @version 1.9.13
 */
class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    /* ── Lifecycle (template methods — own the state machine) ── */

    /**
     * @brief Load model into CPU RAM (COLD → WARM).
     * @param config Model configuration (path, context length, GPU layers, etc.).
     * @return true on success.
     * @version 1.8.2
     */
    bool load(const ModelConfig& config);

    /**
     * @brief Promote to GPU (WARM → ACTIVE). Loads first if COLD.
     * @return true on success.
     * @version 1.8.2
     */
    bool activate();

    /**
     * @brief Release GPU layers (ACTIVE → WARM). No-op if not ACTIVE.
     * @version 1.8.2
     */
    void deactivate();

    /**
     * @brief Full unload (→ COLD). Releases all RAM + VRAM.
     * @version 1.8.2
     */
    void unload();

    /**
     * @brief Convenience: load() + activate().
     * @param config Model configuration passed through to load().
     * @return true on success (both load and activate succeeded).
     * @version 1.8.2
     */
    bool load_and_activate(const ModelConfig& config);

    /* ── Generation (require ACTIVE state) ───────────────── */

    /**
     * @brief Generate a complete response.
     * @param messages Conversation history.
     * @param params Generation parameters.
     * @return GenerationResult with content, token count, timing.
     *         Returns error result if not ACTIVE.
     * @version 1.8.2
     */
    GenerationResult generate(
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /**
     * @brief Generate with per-token streaming callback.
     * @param messages Conversation history.
     * @param params Generation parameters.
     * @param on_token Called for each token (valid only during callback).
     * @param cancel Set to true to abort. Latency: one token.
     * @return GenerationResult with final content and timing.
     * @version 1.8.2
     */
    GenerationResult generate_streaming(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel);

    /**
     * @brief Raw text completion without chat template.
     * @param prompt Raw prompt string (no chat formatting).
     * @param params Generation parameters.
     * @return GenerationResult.
     * @version 1.8.2
     */
    GenerationResult complete(
        const std::string& prompt,
        const GenerationParams& params);

    /* ── Evaluation (require ACTIVE state) ─────────────── */

    /**
     * @brief Evaluate per-token log-probabilities for a token sequence.
     * @param tokens Array of token IDs to evaluate.
     * @param n_tokens Number of tokens in the array (minimum 2).
     * @return LogprobResult with per-token logprobs and perplexity.
     * @throws std::runtime_error if model is not ACTIVE.
     * @throws std::runtime_error if n_tokens < 2.
     *
     * @par Thread safety
     * Serialized by eval_mutex_. Does not block generation.
     * Uses a temporary KV cache sequence ID — no mutation of
     * generation state.
     *
     * @version 1.9.10
     */
    LogprobResult evaluate_logprobs(
        const int32_t* tokens,
        int n_tokens);

    /**
     * @brief Compute perplexity for a token sequence.
     * @param tokens Array of token IDs.
     * @param n_tokens Number of tokens (minimum 2).
     * @return Perplexity as exp(-mean(logprobs)).
     *
     * Convenience method — calls evaluate_logprobs() and returns
     * only the perplexity value.
     *
     * @version 1.9.10
     */
    float compute_perplexity(
        const int32_t* tokens,
        int n_tokens);

    /* ── Queries (lock-free) ─────────────────────────────── */

    /**
     * @brief Current lifecycle state (lock-free read).
     * @return Current ModelState value.
     * @utility
     * @version 1.8.2
     */
    ModelState state() const { return state_.load(std::memory_order_acquire); }

    /**
     * @brief True when state is ACTIVE.
     * @return true if state is ACTIVE, false otherwise.
     * @utility
     * @version 1.8.2
     */
    bool is_active() const { return state() == ModelState::ACTIVE; }

    /**
     * @brief True when state is WARM or ACTIVE.
     * @return true if state is WARM or ACTIVE, false if COLD.
     * @utility
     * @version 1.8.2
     */
    bool is_loaded() const { return state() != ModelState::COLD; }

    /**
     * @brief Count tokens using model's tokenizer.
     * @return Exact count if loaded, len/4 estimate if COLD.
     * @version 1.8.2
     */
    int count_tokens(const std::string& text) const;

    /**
     * @brief Tokenize text to token IDs.
     * @param text Input text.
     * @return Token ID vector (empty if COLD or error).
     * @utility
     * @version 1.10.2
     */
    virtual std::vector<int32_t> tokenize_text(
        const std::string& text) const { return {}; }

    /**
     * @brief Model's context window size.
     * @return Maximum context length in tokens.
     * @utility
     * @version 1.8.2
     */
    int context_length() const { return config_.context_length; }

    /**
     * @brief Invalidate any backend-owned prompt/KV caches.
     *
     * Called when identity or prompt-prefix inputs change so stale
     * cached prefixes are never served against the new system prompt.
     * Default is a no-op for backends with no cache.
     * (P1-7, 2.0.6-rc16)
     *
     * @utility
     * @version 2.0.6-rc16
     */
    virtual void clear_prompt_cache() {}

    /**
     * @brief Stored model config.
     * @return Const reference to the ModelConfig used for this backend.
     * @utility
     * @version 1.8.2
     */
    const ModelConfig& config() const { return config_; }

    /* ── Capability queries (v1.9.13) ────────────────────── */

    /**
     * @brief Query whether this backend supports a capability.
     * @param cap Capability to query.
     * @return true if supported.
     *
     * Base class returns false for all capabilities. Subclasses
     * override do_supports() to declare their capabilities.
     * Lock-free — no state transitions involved.
     *
     * @version 1.9.13
     */
    bool supports(BackendCapability cap) const;

    /**
     * @brief Get all supported capabilities as a vector.
     * @return Vector of capabilities this backend supports.
     *
     * Convenience method. Iterates BackendCapability enum, calls
     * supports() on each.
     *
     * @version 1.9.13
     */
    std::vector<BackendCapability> capabilities() const;

    /* ── Backend metadata (v1.9.13) ──────────────────────── */

    /**
     * @brief Get backend metadata.
     * @return BackendInfo populated from model metadata after load().
     *         Returns default (empty) info with name only if COLD.
     *
     * Base class returns a default-constructed BackendInfo with name
     * from do_backend_name(). Subclasses override do_info() to populate
     * architecture, quantization, memory usage, etc.
     *
     * @version 1.9.13
     */
    BackendInfo info() const;

    /* ── Model state management (v1.9.13) ────────────────── */

    /**
     * @brief Save model state to buffer.
     * @param seq_id Sequence identifier (0 for single-sequence backends).
     * @param buffer Output buffer. Caller owns the returned data.
     * @return true on success. false if not ACTIVE or unsupported.
     *
     * For transformers: saves KV cache state for the sequence.
     * For recurrent: saves hidden state.
     *
     * @version 1.9.13
     */
    bool save_state(int seq_id, std::vector<uint8_t>& buffer) const;

    /**
     * @brief Restore model state from buffer.
     * @param seq_id Sequence identifier to restore into.
     * @param buffer Previously saved state buffer.
     * @return true on success. false if incompatible or unsupported.
     * @version 1.9.13
     */
    bool restore_state(int seq_id, const std::vector<uint8_t>& buffer);

    /**
     * @brief Clear/reset model state for a sequence.
     * @param seq_id Sequence identifier (-1 for all sequences).
     * @return true on success.
     *
     * For transformers: clears KV cache.
     * For recurrent: resets hidden state to initial values.
     *
     * @version 1.9.13
     */
    bool clear_state(int seq_id = -1);

    /* ── Multi-sequence generation (v1.9.13) ─────────────── */

    /**
     * @brief Generate with explicit sequence ID.
     * @param seq_id Sequence identifier for multi-sequence backends.
     * @param messages Conversation history.
     * @param params Generation parameters.
     * @return GenerationResult with seq_id set.
     *
     * Default: ignores seq_id, delegates to generate().
     *
     * @version 1.9.13
     */
    GenerationResult generate_seq(
        int seq_id,
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /**
     * @brief Streaming generation with explicit sequence ID.
     * @param seq_id Sequence identifier.
     * @param messages Conversation history.
     * @param params Generation parameters.
     * @param on_token Per-token callback.
     * @param cancel Cancellation flag.
     * @return GenerationResult with seq_id set.
     * @version 1.9.13
     */
    GenerationResult generate_streaming_seq(
        int seq_id,
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel);

protected:
    /* ── Subclass overrides (20%) ────────────────────────── */

    /**
     * @brief Load model into CPU RAM. Called under transition_mutex_.
     * @param config Validated model config.
     * @return true on success. Set last_error_ on failure.
     * @version 1.8.2
     */
    virtual bool do_load(const ModelConfig& config) = 0;

    /**
     * @brief Promote loaded model to GPU. Called under transition_mutex_.
     * @version 1.8.2
     */
    virtual bool do_activate() = 0;

    /**
     * @brief Release GPU, keep CPU. Called under transition_mutex_.
     * @version 1.8.2
     */
    virtual void do_deactivate() = 0;

    /**
     * @brief Full unload. Called under transition_mutex_.
     * @version 1.8.2
     */
    virtual void do_unload() = 0;

    /**
     * @brief Subclass generation. Called only when ACTIVE.
     * @param messages Conversation history.
     * @param params Generation parameters.
     * @return Generation result populated by the subclass.
     * @version 1.8.2
     */
    virtual GenerationResult do_generate(
        const std::vector<Message>& messages,
        const GenerationParams& params) = 0;

    /**
     * @brief Subclass streaming generation. Called only when ACTIVE.
     * @param messages Conversation history.
     * @param params Generation parameters.
     * @param on_token Callback invoked per emitted token.
     * @param cancel Atomic flag — when true, subclass must stop streaming.
     * @return Generation result populated by the subclass.
     * @version 1.8.2
     */
    virtual GenerationResult do_generate_streaming(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel) = 0;

    /**
     * @brief Subclass raw completion. Called only when ACTIVE.
     * @param prompt Raw prompt string (no chat template applied).
     * @param params Generation parameters.
     * @return Generation result populated by the subclass.
     * @version 1.8.2
     */
    virtual GenerationResult do_complete(
        const std::string& prompt,
        const GenerationParams& params) = 0;

    /**
     * @brief Subclass token counting. Called only when model loaded.
     * @param text Text whose tokens should be counted.
     * @return Token count for the provided text.
     * @version 1.8.2
     */
    virtual int do_count_tokens(const std::string& text) const = 0;

    /**
     * @brief Backend-specific logprob evaluation.
     * @param tokens Token IDs to evaluate.
     * @param n_tokens Number of tokens.
     * @return LogprobResult with per-token logprobs (N-1 values).
     *
     * Called by evaluate_logprobs() after state validation and
     * eval_mutex_ acquisition. The base class handles state
     * assertion, minimum token count validation, mutex, perplexity
     * computation from logprobs, and logging. The implementation
     * handles batch allocation, decode calls, logit extraction,
     * and temporary seq_id lifecycle.
     *
     * @version 1.9.10
     */
    virtual LogprobResult do_evaluate_logprobs(
        const int32_t* tokens,
        int n_tokens) = 0;

    /* ── New overridable methods (v1.9.13) ───────────────── */

    /**
     * @brief Declare supported capabilities.
     * @param cap Capability to check.
     * @return true if this backend supports the capability.
     *
     * Default: returns false for everything.
     *
     * @version 1.9.13
     */
    virtual bool do_supports(BackendCapability cap) const;

    /**
     * @brief Return backend name identifier.
     * @return Short name (e.g. "llama.cpp", "axcl").
     *
     * Pure virtual — every backend must identify itself.
     *
     * @version 1.9.13
     */
    virtual std::string do_backend_name() const = 0;

    /**
     * @brief Populate backend metadata.
     * @return BackendInfo with model-specific details.
     *
     * Default: returns BackendInfo with name from do_backend_name().
     *
     * @version 1.9.13
     */
    virtual BackendInfo do_info() const;

    /**
     * @brief Save model state (KV cache or hidden state).
     * @param seq_id Sequence identifier.
     * @param buffer Output buffer.
     * @return true on success. Default: returns false (not supported).
     * @version 1.9.13
     */
    virtual bool do_save_state(int seq_id,
                               std::vector<uint8_t>& buffer) const;

    /**
     * @brief Restore model state.
     * @param seq_id Sequence identifier.
     * @param buffer State data to restore.
     * @return true on success. Default: returns false (not supported).
     * @version 1.9.13
     */
    virtual bool do_restore_state(int seq_id,
                                  const std::vector<uint8_t>& buffer);

    /**
     * @brief Clear/reset model state.
     * @param seq_id Sequence ID, or -1 for all.
     * @return true on success. Default: returns false (not supported).
     * @version 1.9.13
     */
    virtual bool do_clear_state(int seq_id);

    /**
     * @brief Generate with sequence ID.
     * @param seq_id Sequence identifier.
     * @param messages Conversation history.
     * @param params Generation parameters.
     * @return GenerationResult.
     *
     * Default: ignores seq_id, delegates to do_generate().
     *
     * @version 1.9.13
     */
    virtual GenerationResult do_generate_seq(
        int seq_id,
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /**
     * @brief Streaming generation with sequence ID.
     * @param seq_id Sequence identifier.
     * @param messages Conversation history.
     * @param params Generation parameters.
     * @param on_token Per-token callback.
     * @param cancel Cancellation flag.
     * @return GenerationResult.
     *
     * Default: ignores seq_id, delegates to do_generate_streaming().
     *
     * @version 1.9.13
     */
    virtual GenerationResult do_generate_streaming_seq(
        int seq_id,
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel);

    std::string last_error_;  ///< Last error message for diagnostics

    /**
     * @brief Fire ON_MODEL_LOAD pre-hook.
     * @param config Model config being loaded.
     * @return true if hook cancelled the load.
     * @version 1.9.1
     */
    bool fire_model_load_hook(const ModelConfig& config);  ///< @internal

    /**
     * @brief Set the hook dispatch interface.
     * @param hooks Hook dispatch interface.
     * @utility
     * @version 1.9.1
     */
    void set_hooks(const HookInterface& hooks) { hooks_ = hooks; }

private:
    std::atomic<ModelState> state_{ModelState::COLD};
    ModelConfig config_;
    std::mutex transition_mutex_;  ///< Guards state TRANSITIONS only
    std::mutex eval_mutex_;        ///< Guards evaluation calls (separate from generation) (v1.9.10)
    HookInterface hooks_;          ///< Hook dispatch (v1.9.1)
};

} // namespace entropic
