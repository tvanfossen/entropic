/**
 * @file backend.cpp
 * @brief InferenceBackend base class implementation.
 *
 * Implements the lifecycle state machine (80% logic). All public
 * methods handle state validation, locking, timing, and logging.
 * Subclass do_* methods provide the 20% backend-specific logic.
 *
 * @version 1.8.2
 */

#include <entropic/inference/backend.h>
#include <entropic/types/logging.h>

#include <chrono>
#include <cstdlib>
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

/**
 * @brief Get current time as high-resolution time point.
 * @return Steady clock time point.
 * @utility
 * @version 1.8.2
 */
auto now() { return std::chrono::steady_clock::now(); }

/**
 * @brief Compute elapsed milliseconds between two time points.
 * @param start Start time point.
 * @param end End time point.
 * @return Elapsed time in milliseconds.
 * @utility
 * @version 1.8.2
 */
double elapsed_ms(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end)
{
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    return static_cast<double>(us.count()) / 1000.0;
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
 * @version 1.9.1
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
    auto start = now();

    config_ = config;
    bool ok = do_load(config);
    if (!ok) {
        logger->error("[VRAM] Load failed: {}", last_error_);
    } else {
        state_.store(ModelState::WARM, std::memory_order_release);
        logger->info("[VRAM] Warm in {:.2f}ms", elapsed_ms(start, now()));
    }
    return ok;
}

/**
 * @brief Promote to GPU (WARM → ACTIVE). Loads first if COLD.
 * @return true on success, false on failure.
 * @internal
 * @version 1.8.2
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
    auto start = now();
    bool ok = do_activate();
    if (!ok) {
        logger->error("[VRAM] Activate failed: {}", last_error_);
    } else {
        state_.store(ModelState::ACTIVE, std::memory_order_release);
        logger->info("[VRAM] Active in {:.2f}ms", elapsed_ms(start, now()));
    }
    return ok;
}

/**
 * @brief Release GPU layers (ACTIVE → WARM). No-op if not ACTIVE.
 * @internal
 * @version 1.8.2
 */
void InferenceBackend::deactivate() {
    std::lock_guard<std::mutex> lock(transition_mutex_);

    if (state() != ModelState::ACTIVE) {
        logger->info("[VRAM] deactivate() no-op: {}", state_name(state()));
        return;
    }

    logger->info("[VRAM] Deactivating");
    auto start = now();

    do_deactivate();
    state_.store(ModelState::WARM, std::memory_order_release);

    logger->info("[VRAM] Deactivated in {:.2f}ms", elapsed_ms(start, now()));
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
 * @version 1.8.2
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

    auto start = now();
    auto result = do_generate(messages, params);
    result.generation_time_ms = elapsed_ms(start, now());
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
 * @version 1.8.2
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

    auto start = now();
    auto result = do_generate_streaming(messages, params, on_token, cancel);
    result.generation_time_ms = elapsed_ms(start, now());
    return result;
}

/**
 * @brief Raw text completion. Requires ACTIVE state.
 * @param prompt Raw prompt string.
 * @param params Generation parameters.
 * @return GenerationResult.
 * @internal
 * @version 1.8.2
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

    auto start = now();
    auto result = do_complete(prompt, params);
    result.generation_time_ms = elapsed_ms(start, now());
    return result;
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

} // namespace entropic
