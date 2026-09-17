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

// v2.12.0: adaptive partial-offload split from actual free VRAM.
#include "../../src/inference/device_memory.h"
#include "../../src/inference/partial_offload.h"

/**
 * @brief Host RAM currently available, in bytes (0 when unknown).
 *
 * MemAvailable, not MemFree: the kernel's own estimate of what a new
 * allocation can obtain including reclaimable cache, which is the number that
 * actually predicts whether a load succeeds.
 *
 * @return Available bytes, or 0 when /proc/meminfo cannot be read.
 * @utility
 * @version 2.12.0
 */
inline uint64_t host_available_bytes() {
    std::ifstream mi("/proc/meminfo");
    std::string key;
    uint64_t kb = 0;
    while (mi >> key) {
        if (key == "MemAvailable:") {
            mi >> kb;
            return kb * 1024ull;
        }
        mi.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 0;
}

/**
 * @brief Override the default tier to load a v2.1.9 family GGUF.
 *
 * Resolves the registry key to its on-disk path, checks existence,
 * mutates the default tier's `path` and `adapter` to match the
 * requested model, then calls `init_orchestrator`. If the file is
 * missing (not downloaded), returns false so the test listener can
 * leave `g_ctx.initialized` at its default and SCENARIOs SKIP.
 *
 * gh#149 (v2.13.0): FIVE distinct rules end in `return false` here — an
 * unresolvable registry key, an absent GGUF, the operator waiver, a host that
 * cannot hold the WARM load, and a load that failed with the file present.
 * The bool cannot tell them apart, so each one now also writes
 * `ctx.skip_reason`; the SCENARIOs read that instead of assuming the second.
 *
 * @param ctx Test context to update (including `skip_reason` on failure).
 * @param key Registry key (e.g. "qwen3_6_a3b").
 * @return true on success, false if the GGUF isn't present or the
 *         orchestrator failed to load it.
 * @utility
 * @version 2.13.0
 */
inline bool init_orchestrator_for_v219_family(ModelTestContext& ctx,
                                              const std::string& key) {
    // gh#149: every `return false` below records WHY in ctx.skip_reason, so
    // the SCENARIO's SKIP states the rule that fired rather than assuming the
    // first of five. facts accumulates as each fact becomes known.
    entropic::test::SkipFacts facts;
    facts.key = key;

    const auto* entry = ctx.registry.get(key);
    if (entry == nullptr) {
        spdlog::error("Registry key '{}' not found — check data/bundled_models.yaml",
                      key);
        ctx.skip_reason = entropic::test::skip_reason_text(
            entropic::test::SkipCause::kRegistryKeyMissing, facts);
        return false;
    }

    auto path = ctx.registry.resolve(key);
    facts.path = path.string();
    if (!fs::is_regular_file(path)) {
        spdlog::warn("Model GGUF for '{}' not on disk at {} — "
                     "run `entropic download {}` first. "
                     "Test scenarios will be skipped.",
                     key, path.string(), key);
        ctx.skip_reason = entropic::test::skip_reason_text(
            entropic::test::SkipCause::kGgufMissing, facts);
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

    // gh#86 (v2.6.1): these are tool-call PARSING + GGUF-load fixtures,
    // not thinking-behavior tests. Disable thinking so the model emits
    // its tool call directly instead of deliberating. Before v2.6.1 the
    // low-level chat-template path silently rendered thinking-off, so
    // the battery passed by accident; the v2.6.1 jinja render correctly
    // honors the template's thinking variable, and a verbose 26B (a4b)
    // would otherwise spend the whole max_tokens budget in a <channel>
    // thought block and never reach the tool call. Setting it here both
    // restores deterministic tool-call emission AND exercises the
    // gh#86 wiring end-to-end.
    tier.enable_thinking = false;

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
    // v2.7.0: 20 → 15. At 20 the 13GB A3B/A4B GGUFs need ~8.8GB VRAM +
    // a ~0.5GB compute buffer; with the desktop GPU at ~1.8GB the 11GB
    // 1080 Ti OOM'd the compute-buffer alloc (model-results run7). 15
    // (~6.6GB VRAM) leaves ~2GB headroom for competing desktop GPU —
    // correctness is unaffected by the GPU/CPU offload split.
    std::error_code size_ec;
    auto file_size = fs::file_size(path, size_ec);
    if (!size_ec) { facts.file_bytes = file_size; }
    // v2.12.0: a recurrent/hybrid family is skipped when the host cannot
    // hold the WARM load. The WARM state maps the ENTIRE model into CPU RAM
    // regardless of gpu_layers — measured 12952 MiB for a 13.6 GB GGUF —
    // and only the ACTIVE reload honours the partial-offload split. So peak
    // host usage is the whole file, and on a 31 GB box already holding ~15 GB
    // that is a coin flip rather than a clean failure.
    //
    // Scoped to the hybrid/recurrent Qwen family deliberately: the dense
    // gemma4 26B-A4B is LARGER (13.6 GB vs 12.3 GB) and passes, so this is
    // not a size rule. Gating on size would skip a test that works.
    //
    // Match the ADAPTER ("qwen36"), not the llama.cpp architecture string
    // ("qwen35moe") — the first cut used the arch name, never matched, and
    // the test skipped for an unrelated reason while appearing gated.
    if (!size_ec && file_size > LARGE_GGUF_BYTES
        && entry->adapter == "qwen36") {
        facts.available_bytes = host_available_bytes();
        facts.needed_bytes = file_size + (2ULL * 1024 * 1024 * 1024);
        // gh#149: these are TWO rules, and until v2.13.0 both logged the RAM
        // text — so an explicit operator allowance was recorded as a host
        // that had run out of memory. An allowance is a decision someone
        // made; a shortfall is a measurement. They read differently now.
        const bool waived = entropic::large_model_tests_waived(file_size);
        const bool fits = entropic::host_can_hold_warm_load(
            file_size, facts.available_bytes);
        if (waived || !fits) {
            ctx.skip_reason = entropic::test::skip_reason_text(
                waived ? entropic::test::SkipCause::kLargeModelWaived
                       : entropic::test::SkipCause::kHostRamInsufficient,
                facts);
            // One string, logged and reported — they cannot drift apart.
            spdlog::warn("v2.1.9 family: SKIPPING — {}", ctx.skip_reason);
            return false;
        }
    }
    if (!size_ec && file_size > LARGE_GGUF_BYTES) {
        // v2.12.0: derive the split from ACTUAL free VRAM instead of a
        // hardcoded 15. That constant came from a v2.7.0 measurement taken
        // while the desktop held ~1.8 GB of VRAM, and it never adapted: on a
        // quiet card reporting 10.2 GB free it still pinned 15 of 40 layers
        // to the GPU (~5 GB) and left ~8.2 GB in SYSTEM RAM, which is what
        // the low-memory watchdog killed.
        //
        // The direction matters and is easy to get backwards: every layer
        // moved OFF the GPU adds ~330 MB to system RAM for this model, so
        // "more CPU offload" makes an OOM worse, not better. Fitting more
        // into VRAM is what reduces the host-side footprint.
        tier.gpu_layers = entropic::partial_gpu_layers_for(
            file_size, entropic::query_device_free_vram_bytes());
        // v2.12.0: and UNPIN it. Clamping gpu_layers puts ~6.6 GB of a 13 GB
        // GGUF on the CPU side, but use_mlock defaults to true, so llama.cpp
        // tries to lock that portion into unevictable RAM — locking up to
        // RLIMIT_MEMLOCK (3.9 GB here) before warning "failed to mlock ...
        // Cannot allocate memory".
        //
        // Pinned pages cannot be reclaimed under pressure, which is what
        // turns partial offload from SLOW into FATAL: the suite was
        // OOM-killed loading this model rather than paging through it. The
        // whole point of the accommodation is that a model too large for the
        // card still RUNS, just slowly — and that requires it to be
        // pageable. mlock is an optimisation for models that comfortably
        // fit; for one that does not, it is precisely wrong.
        tier.use_mlock = false;
        spdlog::warn("v2.1.9 family: '{}' GGUF is {} bytes (>{} GB) — "
                     "fitting {} layers to {} MiB free VRAM and disabling "
                     "mlock so the CPU-side portion stays pageable "
                     "(dev-box small-VRAM accommodation)",
                     key, file_size,
                     LARGE_GGUF_BYTES / (1024ULL * 1024 * 1024),
                     tier.gpu_layers,
                     entropic::query_device_free_vram_bytes()
                         / (1024ULL * 1024));
    }

    ctx.model_path = path.string();

    spdlog::info("v2.1.9 family override: tier={} key={} adapter={} "
                 "context_length={} gpu_layers={} path={}",
                 ctx.config.models.default_tier, key, entry->adapter,
                 V219_TEST_CTX, tier.gpu_layers, path.string());
    if (!init_orchestrator(ctx)) {
        // gh#149: init_orchestrator already recorded a load failure, but it
        // only knows the TIER name. Restate it with the registry key the
        // test actually asked for — and keep it a LOAD failure, since the
        // GGUF was proven present twenty lines above.
        ctx.skip_reason = entropic::test::skip_reason_text(
            entropic::test::SkipCause::kOrchestratorInitFailed, facts);
        return false;
    }
    return true;
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
        // gh#149: load_harness_inputs records WHICH setup step failed, so a
        // bad bundled_models.yaml is not reported as a missing download.
        bool ok = load_harness_inputs(g_ctx, Key);
        ok = ok && init_orchestrator_for_v219_family(g_ctx, Key);
        if (!ok) {
            spdlog::warn("v2.1.9 family setup did not complete for '{}' — "
                         "test SCENARIOs will SKIP: {}", Key,
                         g_ctx.skip_reason);
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
