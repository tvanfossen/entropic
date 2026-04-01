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
 * @version 1.9.10
 */

#pragma once

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
 * @version 1.8.2
 */
class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    /* ── Lifecycle (template methods — own the state machine) ── */

    /**
     * @brief Load model into CPU RAM (COLD → WARM).
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
     * @return true on success.
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
     * @version 1.8.2
     */
    ModelState state() const { return state_.load(std::memory_order_acquire); }

    /**
     * @brief True when state is ACTIVE.
     * @version 1.8.2
     */
    bool is_active() const { return state() == ModelState::ACTIVE; }

    /**
     * @brief True when state is WARM or ACTIVE.
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
     * @brief Model's context window size.
     * @version 1.8.2
     */
    int context_length() const { return config_.context_length; }

    /**
     * @brief Stored model config.
     * @version 1.8.2
     */
    const ModelConfig& config() const { return config_; }

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
     * @version 1.8.2
     */
    virtual GenerationResult do_generate(
        const std::vector<Message>& messages,
        const GenerationParams& params) = 0;

    /**
     * @brief Subclass streaming generation. Called only when ACTIVE.
     * @version 1.8.2
     */
    virtual GenerationResult do_generate_streaming(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel) = 0;

    /**
     * @brief Subclass raw completion. Called only when ACTIVE.
     * @version 1.8.2
     */
    virtual GenerationResult do_complete(
        const std::string& prompt,
        const GenerationParams& params) = 0;

    /**
     * @brief Subclass token counting. Called only when model loaded.
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
