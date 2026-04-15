// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file backend.cpp
 * @brief InferenceBackend base class implementation.
 *
 * Implements the lifecycle state machine (80% logic). All public
 * methods handle state validation, locking, timing, and logging.
 * Subclass do_* methods provide the 20% backend-specific logic.
 *
 * @version 1.9.13
 */

#include <entropic/inference/backend.h>
#include <entropic/types/logging.h>

#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace entropic {

namespace {

auto logger = entropic::log::get("inference.backend");

/**
 * @brief Convert ModelState to string for logging.
 * @param s Model state.
 * @return String representation.
 * @utility
 * @version 1.8.2
 */
const char* state_name(ModelState s) {
    static constexpr const char* names[] = {"COLD", "WARM", "ACTIVE"};
    int idx = static_cast<int>(s);
    return (idx >= 0 && idx <= 2) ? names[idx] : "UNKNOWN";
}

} // anonymous namespace

// ── Lifecycle ──────────────────────────────────────────────

/**
 * @brief Load model into CPU RAM (COLD → WARM).
 *
 * Acquires transition_mutex_. No-op if already WARM/ACTIVE.
 *
 * @param config Validated model config.
 * @return true on success, false on failure.
 * @internal
 * @version 2.0.0
 */
bool InferenceBackend::load(const ModelConfig& config) {
    std::lock_guard<std::mutex> lock(transition_mutex_);

    if (state() != ModelState::COLD) {
        logger->info("[VRAM] load() no-op: already {}", state_name(state()));
        return true;
    }

    // Hook: ON_MODEL_LOAD — can cancel (v1.9.1)
    bool cancelled = fire_model_load_hook(config);
    if (cancelled) {
        return false;
    }

    logger->info("[VRAM] Loading: {}", config.path.string());
    auto start = entropic::log::now();

    config_ = config;
    bool ok = do_load(config);
    if (!ok) {
        logger->error("[VRAM] Load failed: {}", last_error_);
    } else {
        state_.store(ModelState::WARM, std::memory_order_release);
        logger->info("[VRAM] Warm in {:.2f}ms", entropic::log::elapsed_ms(start, entropic::log::now()));
    }
    return ok;
}

/**
 * @brief Promote to GPU (WARM → ACTIVE). Loads first if COLD.
 * @return true on success, false on failure.
 * @internal
 * @version 2.0.0
 */
bool InferenceBackend::activate() {
    std::lock_guard<std::mutex> lock(transition_mutex_);

    if (state() == ModelState::ACTIVE) {
        logger->info("[VRAM] activate() no-op: already ACTIVE");
        return true;
    }
    if (state() != ModelState::WARM) {
        logger->error("[VRAM] activate() failed: not WARM ({})", state_name(state()));
        return false;
    }

    logger->info("[VRAM] Activating");
    auto start = entropic::log::now();
    bool ok = do_activate();
    if (!ok) {
        logger->error("[VRAM] Activate failed: {}", last_error_);
    } else {
        state_.store(ModelState::ACTIVE, std::memory_order_release);
        logger->info("[VRAM] Active in {:.2f}ms", entropic::log::elapsed_ms(start, entropic::log::now()));
    }
    return ok;
}

/**
 * @brief Release GPU layers (ACTIVE → WARM). No-op if not ACTIVE.
 * @internal
 * @version 2.0.0
 */
void InferenceBackend::deactivate() {
    std::lock_guard<std::mutex> lock(transition_mutex_);

    if (state() != ModelState::ACTIVE) {
        logger->info("[VRAM] deactivate() no-op: {}", state_name(state()));
        return;
    }

    logger->info("[VRAM] Deactivating");
    auto start = entropic::log::now();

    do_deactivate();
    state_.store(ModelState::WARM, std::memory_order_release);

    logger->info("[VRAM] Deactivated in {:.2f}ms", entropic::log::elapsed_ms(start, entropic::log::now()));
}

/**
 * @brief Full unload (→ COLD). Idempotent.
 * @internal
 * @version 1.9.1
 */
void InferenceBackend::unload() {
    std::lock_guard<std::mutex> lock(transition_mutex_);

    // Hook: ON_MODEL_UNLOAD — informational (v1.9.1)
    if (hooks_.fire_info != nullptr) {
        std::string json = "{\"state\":\""
            + std::string(state_name(state())) + "\"}";
        hooks_.fire_info(hooks_.registry,
            ENTROPIC_HOOK_ON_MODEL_UNLOAD, json.c_str());
    }

    logger->info("[VRAM] Unloading from {}", state_name(state()));

    do_unload();
    state_.store(ModelState::COLD, std::memory_order_release);

    logger->info("[VRAM] Unloaded");
}

/**
 * @brief Convenience: load() + activate().
 * @param config Model config.
 * @return true on success.
 * @internal
 * @version 1.8.2
 */
bool InferenceBackend::load_and_activate(const ModelConfig& config) {
    if (!load(config)) {
        return false;
    }
    return activate();
}

// ── Generation ─────────────────────────────────────────────

/**
 * @brief Generate a complete response. Requires ACTIVE state.
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @return GenerationResult (error result if not ACTIVE).
 * @internal
 * @version 2.0.0
 */
GenerationResult InferenceBackend::generate(
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    if (!is_active()) {
        GenerationResult err;
        err.error_code = ENTROPIC_ERROR_INVALID_STATE;
        err.error_message = "generate() requires ACTIVE state";
        err.finish_reason = "error";
        logger->error("{}", err.error_message);
        return err;
    }

    auto start = entropic::log::now();
    auto result = do_generate(messages, params);
    result.generation_time_ms = entropic::log::elapsed_ms(start, entropic::log::now());
    return result;
}

/**
 * @brief Streaming generation. Requires ACTIVE state.
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param on_token Per-token callback.
 * @param cancel Atomic cancel flag.
 * @return GenerationResult.
 * @internal
 * @version 2.0.0
 */
GenerationResult InferenceBackend::generate_streaming(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view token)> on_token,
    std::atomic<bool>& cancel)
{
    if (!is_active()) {
        GenerationResult err;
        err.error_code = ENTROPIC_ERROR_INVALID_STATE;
        err.error_message = "generate_streaming() requires ACTIVE state";
        err.finish_reason = "error";
        logger->error("{}", err.error_message);
        return err;
    }

    auto start = entropic::log::now();
    auto result = do_generate_streaming(messages, params, on_token, cancel);
    result.generation_time_ms = entropic::log::elapsed_ms(start, entropic::log::now());
    return result;
}

/**
 * @brief Raw text completion. Requires ACTIVE state.
 * @param prompt Raw prompt string.
 * @param params Generation parameters.
 * @return GenerationResult.
 * @internal
 * @version 2.0.0
 */
GenerationResult InferenceBackend::complete(
    const std::string& prompt,
    const GenerationParams& params)
{
    if (!is_active()) {
        GenerationResult err;
        err.error_code = ENTROPIC_ERROR_INVALID_STATE;
        err.error_message = "complete() requires ACTIVE state";
        err.finish_reason = "error";
        logger->error("{}", err.error_message);
        return err;
    }

    auto start = entropic::log::now();
    auto result = do_complete(prompt, params);
    result.generation_time_ms = entropic::log::elapsed_ms(start, entropic::log::now());
    return result;
}

// ── Evaluation (v1.9.10) ───────────────────────────────────

/**
 * @brief Evaluate per-token log-probabilities. Requires ACTIVE state.
 *
 * The 80% logic: state check, input validation, eval_mutex_,
 * perplexity computation from raw logprobs, and logging. Delegates
 * to do_evaluate_logprobs() for backend-specific batch/decode work.
 *
 * @param tokens Array of token IDs.
 * @param n_tokens Number of tokens (minimum 2).
 * @return LogprobResult with per-token logprobs and perplexity.
 * @throws std::runtime_error on state/input errors.
 * @internal
 * @version 2.0.0
 */
LogprobResult InferenceBackend::evaluate_logprobs(
    const int32_t* tokens,
    int n_tokens)
{
    if (!is_active()) {
        logger->error("evaluate_logprobs: model not ACTIVE (state={})",
                      state_name(state()));
        throw std::runtime_error("Model must be ACTIVE for evaluation");
    }

    if (n_tokens < 2) {
        logger->error("evaluate_logprobs: need >= 2 tokens, got {}",
                      n_tokens);
        throw std::runtime_error(
            "Need at least 2 tokens for logprob evaluation");
    }

    std::lock_guard<std::mutex> lock(eval_mutex_);

    logger->info("evaluate_logprobs: {} tokens, first=[{},{},{}...]",
                 n_tokens, tokens[0],
                 n_tokens > 1 ? tokens[1] : 0,
                 n_tokens > 2 ? tokens[2] : 0);
    auto start = entropic::log::now();

    LogprobResult result = do_evaluate_logprobs(tokens, n_tokens);

    result.total_logprob = 0.0f;
    for (float lp : result.logprobs) {
        result.total_logprob += lp;
    }
    float mean_lp = result.total_logprob /
        static_cast<float>(result.n_logprobs);
    result.perplexity = std::exp(-mean_lp);

    auto ms = entropic::log::elapsed_ms(start, entropic::log::now());
    logger->info("evaluate_logprobs: perplexity={:.2f}, "
                 "total_lp={:.4f}, {:.2f}ms",
                 result.perplexity, result.total_logprob, ms);
    for (int i = 0; i < result.n_logprobs; ++i) {
        logger->info("  logprob[{}]={:.4f}", i, result.logprobs[i]);
    }

    return result;
}

/**
 * @brief Compute perplexity for a token sequence.
 *
 * Convenience wrapper — calls evaluate_logprobs() and returns
 * only the perplexity value.
 *
 * @param tokens Array of token IDs.
 * @param n_tokens Number of tokens (minimum 2).
 * @return Perplexity as exp(-mean(logprobs)).
 * @internal
 * @version 1.9.10
 */
float InferenceBackend::compute_perplexity(
    const int32_t* tokens,
    int n_tokens)
{
    return evaluate_logprobs(tokens, n_tokens).perplexity;
}

// ── Hook helpers (v1.9.1) ──────────────────────────────────

/**
 * @brief Fire ON_MODEL_LOAD pre-hook.
 * @param config Model config being loaded.
 * @return true if hook cancelled the load.
 * @internal
 * @version 1.9.1
 */
bool InferenceBackend::fire_model_load_hook(const ModelConfig& config) {
    if (hooks_.fire_pre == nullptr) {
        return false;
    }
    std::string json = "{\"model_path\":\""
        + config.path.string() + "\"}";
    char* mod = nullptr;
    int rc = hooks_.fire_pre(hooks_.registry,
        ENTROPIC_HOOK_ON_MODEL_LOAD, json.c_str(), &mod);
    free(mod);
    if (rc != 0) {
        logger->info("[VRAM] ON_MODEL_LOAD hook cancelled");
    }
    return rc != 0;
}

// ── Queries ────────────────────────────────────────────────

/**
 * @brief Count tokens. Exact if loaded, estimate if COLD.
 * @param text Text to tokenize.
 * @return Token count.
 * @internal
 * @version 1.8.2
 */
int InferenceBackend::count_tokens(const std::string& text) const {
    if (is_loaded()) {
        return do_count_tokens(text);
    }
    return static_cast<int>(text.size()) / 4;
}

// ── Capability queries (v1.9.13) ───────────────────────────

/**
 * @brief Query backend capability. Delegates to do_supports().
 * @param cap Capability to query.
 * @return true if supported.
 * @internal
 * @version 1.9.13
 */
bool InferenceBackend::supports(BackendCapability cap) const {
    return do_supports(cap);
}

/**
 * @brief Get all supported capabilities.
 * @return Vector of supported capabilities.
 * @internal
 * @version 1.9.13
 */
std::vector<BackendCapability> InferenceBackend::capabilities() const {
    std::vector<BackendCapability> result;
    int count = static_cast<int>(BackendCapability::_COUNT);
    for (int i = 0; i < count; ++i) {
        auto cap = static_cast<BackendCapability>(i);
        if (supports(cap)) {
            result.push_back(cap);
        }
    }
    return result;
}

/**
 * @brief Get backend metadata. Delegates to do_info().
 * @return BackendInfo with at least name populated.
 * @internal
 * @version 1.9.13
 */
BackendInfo InferenceBackend::info() const {
    return do_info();
}

// ── Model state management (v1.9.13) ──────────────────────

/**
 * @brief Save model state. Requires ACTIVE.
 * @param seq_id Sequence identifier.
 * @param buffer Output buffer.
 * @return true on success.
 * @internal
 * @version 2.0.0
 */
bool InferenceBackend::save_state(
    int seq_id, std::vector<uint8_t>& buffer) const
{
    if (!is_active()) {
        logger->warn("save_state: not ACTIVE ({})", state_name(state()));
        return false;
    }
    auto start = entropic::log::now();
    bool ok = do_save_state(seq_id, buffer);
    if (ok) {
        logger->info("save_state: seq={} {}B {:.2f}ms",
                     seq_id, buffer.size(), entropic::log::elapsed_ms(start, entropic::log::now()));
    }
    return ok;
}

/**
 * @brief Restore model state. Requires ACTIVE.
 * @param seq_id Sequence identifier.
 * @param buffer Previously saved state.
 * @return true on success.
 * @internal
 * @version 2.0.0
 */
bool InferenceBackend::restore_state(
    int seq_id, const std::vector<uint8_t>& buffer)
{
    if (!is_active()) {
        logger->warn("restore_state: not ACTIVE ({})",
                     state_name(state()));
        return false;
    }
    auto start = entropic::log::now();
    bool ok = do_restore_state(seq_id, buffer);
    if (ok) {
        logger->info("restore_state: seq={} {}B {:.2f}ms",
                     seq_id, buffer.size(), entropic::log::elapsed_ms(start, entropic::log::now()));
    }
    return ok;
}

/**
 * @brief Clear model state. Requires WARM or ACTIVE.
 * @param seq_id Sequence ID, or -1 for all.
 * @return true on success.
 * @internal
 * @version 1.9.13
 */
bool InferenceBackend::clear_state(int seq_id) {
    if (state() == ModelState::COLD) {
        logger->warn("clear_state: model is COLD");
        return false;
    }
    bool ok = do_clear_state(seq_id);
    if (ok) {
        logger->info("clear_state: seq={}", seq_id);
    }
    return ok;
}

// ── Multi-sequence generation (v1.9.13) ────────────────────

/**
 * @brief Generate with explicit sequence ID. Requires ACTIVE.
 * @param seq_id Sequence identifier.
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @return GenerationResult with seq_id set.
 * @internal
 * @version 2.0.0
 */
GenerationResult InferenceBackend::generate_seq(
    int seq_id,
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    if (!is_active()) {
        GenerationResult err;
        err.error_code = ENTROPIC_ERROR_INVALID_STATE;
        err.error_message = "generate_seq() requires ACTIVE state";
        err.finish_reason = "error";
        logger->error("{}", err.error_message);
        return err;
    }

    auto start = entropic::log::now();
    auto result = do_generate_seq(seq_id, messages, params);
    result.generation_time_ms = entropic::log::elapsed_ms(start, entropic::log::now());
    result.seq_id = seq_id;
    return result;
}

/**
 * @brief Streaming generation with sequence ID. Requires ACTIVE.
 * @param seq_id Sequence identifier.
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param on_token Per-token callback.
 * @param cancel Cancellation flag.
 * @return GenerationResult with seq_id set.
 * @internal
 * @version 2.0.0
 */
GenerationResult InferenceBackend::generate_streaming_seq(
    int seq_id,
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view token)> on_token,
    std::atomic<bool>& cancel)
{
    if (!is_active()) {
        GenerationResult err;
        err.error_code = ENTROPIC_ERROR_INVALID_STATE;
        err.error_message =
            "generate_streaming_seq() requires ACTIVE state";
        err.finish_reason = "error";
        logger->error("{}", err.error_message);
        return err;
    }

    auto start = entropic::log::now();
    auto result = do_generate_streaming_seq(
        seq_id, messages, params, on_token, cancel);
    result.generation_time_ms = entropic::log::elapsed_ms(start, entropic::log::now());
    result.seq_id = seq_id;
    return result;
}

// ── Default virtual implementations (v1.9.13) ─────────────

/**
 * @brief Default: no capabilities supported.
 * @param cap Capability to check.
 * @return false.
 * @internal
 * @version 1.9.13
 */
bool InferenceBackend::do_supports(BackendCapability /*cap*/) const {
    return false;
}

/**
 * @brief Default: BackendInfo with name only.
 * @return BackendInfo with name from do_backend_name().
 * @internal
 * @version 1.9.13
 */
BackendInfo InferenceBackend::do_info() const {
    BackendInfo bi;
    bi.name = do_backend_name();
    return bi;
}

/**
 * @brief Default: state save not supported.
 * @param seq_id Sequence identifier.
 * @param buffer Output buffer.
 * @return false.
 * @internal
 * @version 1.9.13
 */
bool InferenceBackend::do_save_state(
    int /*seq_id*/, std::vector<uint8_t>& /*buffer*/) const
{
    return false;
}

/**
 * @brief Default: state restore not supported.
 * @param seq_id Sequence identifier.
 * @param buffer State data.
 * @return false.
 * @internal
 * @version 1.9.13
 */
bool InferenceBackend::do_restore_state(
    int /*seq_id*/, const std::vector<uint8_t>& /*buffer*/)
{
    return false;
}

/**
 * @brief Default: state clear not supported.
 * @param seq_id Sequence identifier.
 * @return false.
 * @internal
 * @version 1.9.13
 */
bool InferenceBackend::do_clear_state(int /*seq_id*/) {
    return false;
}

/**
 * @brief Default: ignores seq_id, delegates to do_generate().
 * @param seq_id Sequence identifier (ignored).
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @return GenerationResult from do_generate().
 * @internal
 * @version 1.9.13
 */
GenerationResult InferenceBackend::do_generate_seq(
    int /*seq_id*/,
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    return do_generate(messages, params);
}

/**
 * @brief Default: ignores seq_id, delegates to do_generate_streaming().
 * @param seq_id Sequence identifier (ignored).
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param on_token Per-token callback.
 * @param cancel Cancellation flag.
 * @return GenerationResult from do_generate_streaming().
 * @internal
 * @version 1.9.13
 */
GenerationResult InferenceBackend::do_generate_streaming_seq(
    int /*seq_id*/,
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view token)> on_token,
    std::atomic<bool>& cancel)
{
    return do_generate_streaming(messages, params, on_token, cancel);
}

} // namespace entropic
