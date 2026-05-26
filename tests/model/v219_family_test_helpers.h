// SPDX-License-Identifier: Apache-2.0
/**
 * @file v219_family_test_helpers.h
 * @brief Helpers for v2.1.9 family model tests (gh#44/#45/#46/#47).
 *
 * Each new-family model test (Qwen 3.6, Gemma 4, Nemotron 3) loads a
 * different bundled GGUF than the default `primary`. This header
 * provides a single setup path that overrides the default tier in
 * g_ctx.config to point at a specified registry key and adapter,
 * then drives `init_orchestrator` from the existing model test
 * infrastructure. If the GGUF isn't present on disk (developer
 * hasn't run `entropic download <key>` yet), setup is reported as
 * unsuccessful and SCENARIOs SKIP cleanly instead of failing.
 *
 * @version 2.1.9
 */

#pragma once

#include "model_test_context.h"

/**
 * @brief Override the default tier to load a v2.1.9 family GGUF.
 *
 * Resolves the registry key to its on-disk path, checks existence,
 * mutates the default tier's `path` and `adapter` to match the
 * requested model, then calls `init_orchestrator`. If the file is
 * missing (not downloaded), returns false so the test listener can
 * leave `g_ctx.initialized` at its default and SCENARIOs SKIP.
 *
 * @param ctx Test context to update.
 * @param key Registry key (e.g. "qwen3_6_a3b").
 * @return true on success, false if the GGUF isn't present or the
 *         orchestrator failed to load it.
 * @utility
 * @version 2.1.9
 */
inline bool init_orchestrator_for_v219_family(ModelTestContext& ctx,
                                              const std::string& key) {
    const auto* entry = ctx.registry.get(key);
    if (entry == nullptr) {
        spdlog::error("Registry key '{}' not found — check data/bundled_models.yaml",
                      key);
        return false;
    }

    auto path = ctx.registry.resolve(key);
    if (!fs::is_regular_file(path)) {
        spdlog::warn("Model GGUF for '{}' not on disk at {} — "
                     "run `entropic download {}` first. "
                     "Test scenarios will be skipped.",
                     key, path.string(), key);
        return false;
    }

    auto& tier = ctx.config.models.tiers[ctx.config.models.default_tier];
    tier.path = path;
    tier.adapter = entry->adapter;
    // v2.1.9 family tests are smoke + single-shot tool-call fixtures —
    // prompts are tens of tokens and max_tokens is 256. Override the
    // tier's default context_length (typically 32K) down to a small
    // value so KV cache fits on the dev GPU even for the largest
    // model (Gemma 4 A4B, 26B params + ~6 GB KV at 4K context).
    // Production deployments load the full context via config; this
    // override only scopes the test harness.
    constexpr int V219_TEST_CTX = 4096;
    tier.context_length = V219_TEST_CTX;

    // v2.4.0 (gh#23 MVP-10 follow-up): when the family GGUF is too
    // large for the host GPU's VRAM, llama_model_load_from_file with
    // n_gpu_layers=-1 fails outright (no graceful "fit as many as
    // possible" mode). Detect oversized GGUFs by file size and clamp
    // gpu_layers to a partial-offload value that keeps a 16-bit
    // hot-layer set on the GPU while the rest stream from CPU. This
    // preserves the v219 contract on dev boxes with smaller GPUs
    // (1080 Ti, 11 GB) without affecting prod where VRAM is sized
    // for the model — production configs override `gpu_layers`
    // explicitly via YAML.
    //
    // Threshold rationale: ~10 GB matches gemma4_a4b (13.6 GB) and
    // qwen3_6_a3b (13.2 GB) on the upper side; the e4b (4.5 GB),
    // e4b_q4 (4.8 GB), e2b (2.5 GB), nemotron3 (3.1 GB) all stay
    // well below and continue full GPU offload (the prior behavior).
    constexpr uintmax_t LARGE_GGUF_BYTES = 10ULL * 1024 * 1024 * 1024;
    constexpr int PARTIAL_GPU_LAYERS = 20;
    std::error_code size_ec;
    auto file_size = fs::file_size(path, size_ec);
    if (!size_ec && file_size > LARGE_GGUF_BYTES) {
        tier.gpu_layers = PARTIAL_GPU_LAYERS;
        spdlog::warn("v2.1.9 family: '{}' GGUF is {} bytes (>{} GB) — "
                     "clamping gpu_layers to {} for partial CPU offload "
                     "(dev-box small-VRAM accommodation)",
                     key, file_size,
                     LARGE_GGUF_BYTES / (1024ULL * 1024 * 1024),
                     PARTIAL_GPU_LAYERS);
    }

    ctx.model_path = path.string();

    spdlog::info("v2.1.9 family override: tier={} key={} adapter={} "
                 "context_length={} gpu_layers={} path={}",
                 ctx.config.models.default_tier, key, entry->adapter,
                 V219_TEST_CTX, tier.gpu_layers, path.string());
    return init_orchestrator(ctx);
}

/**
 * @brief Catch2 listener factory for a v2.1.9 family test executable.
 *
 * Customised testRunStarting that loads the registry + config, then
 * overrides the default tier with the requested family key before
 * initialising the orchestrator. Use `using` to bind a specific key
 * inside a test file, then `CATCH_REGISTER_LISTENER` the result.
 *
 * @version 2.1.9
 */
template <const char* Key>
class V219FamilyListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    /**
     * @brief Load registry + config, override tier to Key, init orchestrator.
     * @utility
     * @version 2.1.9
     */
    void testRunStarting(Catch::TestRunInfo const& /*info*/) override {
        spdlog::info("Loading v2.1.9 family model: key={}", Key);
        fs::create_directories(LOG_DIR);
        bool ok = load_registry(g_ctx.registry);
        ok = ok && load_test_config(g_ctx.registry, g_ctx.config);
        ok = ok && init_orchestrator_for_v219_family(g_ctx, Key);
        if (!ok) {
            spdlog::warn("v2.1.9 family setup did not complete for '{}' — "
                         "test SCENARIOs will SKIP.", Key);
        }
    }

    /**
     * @brief Clear prompt/KV caches before each scenario for isolation.
     *
     * The orchestrator is loaded once and shared across scenarios; under
     * Catch2's randomized scenario order, a prior scenario's cached prompt
     * prefix could otherwise condition the next scenario's generation
     * (e.g. the smoke math answer flipped when the toolcall battery ran
     * first). Clearing per case makes every scenario start from a clean
     * model state regardless of order.
     *
     * @utility
     * @version 2.3.8
     */
    void testCaseStarting(Catch::TestCaseInfo const& /*tc*/) override {
        if (g_ctx.orchestrator) {
            g_ctx.orchestrator->clear_all_prompt_caches();
        }
    }

    /**
     * @brief Shutdown orchestrator at end of run if it loaded.
     * @utility
     * @version 2.1.9
     */
    void testRunEnded(Catch::TestRunStats const& /*stats*/) override {
        if (g_ctx.orchestrator) {
            g_ctx.orchestrator->shutdown();
        }
    }
};
