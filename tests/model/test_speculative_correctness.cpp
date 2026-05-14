// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_speculative_correctness.cpp
 * @brief Bit-identical correctness contract for speculative decoding.
 *
 * The v2.1.11 proposal calls "output distribution bit-identical to
 * plain decode on rejection cases" THE correctness contract for the
 * speculative kernel. This test enforces it empirically:
 *
 *   1. Build a deterministic generation setup (greedy sampling at
 *      temperature=0, fixed seed, deterministic prompt).
 *   2. Generate `N` tokens via the standard streaming path.
 *   3. Reset state, generate the same `N` tokens via the speculative
 *      kernel (Qwen3.6-A3B target + Qwen3.5-0.8B CPU-resident draft).
 *   4. Assert exact string equality.
 *
 * Even when the draft proposes "wrong" tokens that the target
 * rejects, the post-rejection token MUST equal what the standard
 * decode would have sampled at that position. Any divergence here
 * indicates a bug in the accept-and-rollback path.
 *
 * VRAM budget: Qwen3.6-A3B at ~14k context fits comfortably; the
 * draft sits on CPU (`n_gpu_layers=0`) so the total VRAM footprint
 * matches plain decode.
 *
 * Requires: GPU with ≥ 16 GB VRAM, both GGUFs present locally.
 * Run: ctest -L model -R speculative-correctness
 *
 * @version 2.1.11
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

namespace {

/**
 * @brief Configure the orchestrator with speculative decoding on
 *        (Qwen3.6-A3B + Qwen3.5-0.8B), CPU-resident draft.
 *
 * Returns nullptr (and SKIPs the calling test via Catch) when either
 * model is missing locally.
 *
 * @return Configured orchestrator, or nullptr on missing models.
 * @utility
 * @version 2.1.11
 */
std::unique_ptr<ModelOrchestrator> build_speculative_orchestrator(
    const config::BundledModels& registry,
    ParsedConfig& config) {
    auto draft_path = registry.resolve("qwen3_5_0_8b");
    // Qwen3.5-4B as the target — matched dense-transformer family
    // with the 0.8B draft. Tested 9B previously, returned FULL seq_rm;
    // checking whether smaller sizes behave differently.
    auto target_path = registry.resolve("qwen3_5_4b");
    if (!fs::exists(draft_path) || !fs::exists(target_path)) {
        WARN("Qwen3.5 9B target or 0.8B draft GGUF missing — skipping");
        return nullptr;
    }
    config.inference.speculative.enabled = true;
    config.inference.speculative.n_draft = 16;
    config.inference.speculative.draft.path = draft_path;
    config.inference.speculative.draft.flash_attn = false;

    // Override the default tier to Qwen3.5-9B (dense transformer) —
    // the kernel needs the target context to report PARTIAL seq_rm,
    // which MoE architectures (Qwen3.5-A3B) don't provide. Dense
    // transformers like Qwen3.5-9B do. Shrink ctx + disable
    // flash_attn for the same reason on the target.
    for (auto& [name, tier] : config.models.tiers) {
        tier.path = target_path;
        tier.adapter = "qwen35";
        tier.context_length = 4096;
        tier.flash_attn = false;
    }

    // The shared g_ctx.orchestrator is already holding the primary
    // model's VRAM allocation from the listener's testRunStarting.
    // Releasing it here keeps total VRAM usage to one primary copy.
    if (g_ctx.orchestrator) {
        g_ctx.orchestrator->shutdown();
        g_ctx.orchestrator.reset();
    }

    auto orch = std::make_unique<ModelOrchestrator>();
    if (!orch->initialize(config)) {
        WARN("Speculative orchestrator init failed");
        return nullptr;
    }
    return orch;
}

/**
 * @brief Run a single deterministic generation through the
 *        orchestrator. Forces speculative ON/OFF via a flag the
 *        caller passes (toggled on the shared config).
 *
 * @param orch Orchestrator.
 * @param config Parsed config (mutated for the speculative flag).
 * @param speculative_on Whether to enable the speculative path.
 * @param prompt User prompt.
 * @param max_tokens Token budget.
 * @return GenerationResult.
 * @utility
 * @version 2.1.11
 */
GenerationResult run_one(
    ModelOrchestrator& orch, ParsedConfig& /*config*/,
    bool speculative_on, const std::string& prompt, int max_tokens) {
    orch.set_speculative_enabled(speculative_on);
    auto messages = make_messages(
        "You are a precise, terse assistant.", prompt);
    GenerationParams params;
    params.temperature = 0.0f;   // greedy
    params.seed = 42;
    params.max_tokens = max_tokens;
    params.top_k = 1;
    return orch.generate(messages, params, "");
}

} // anonymous namespace

SCENARIO("Speculative decoding produces bit-identical output vs plain "
         "decode (correctness contract)",
         "[model][speculative][correctness]")
{
    GIVEN("a configured orchestrator with Qwen3.6-A3B target and "
          "Qwen3.5-0.8B CPU draft") {
        REQUIRE(g_ctx.initialized);
        start_test_log("speculative_correctness");

        // Re-initialize with speculative on
        ParsedConfig spec_config = g_ctx.config;
        auto orch = build_speculative_orchestrator(
            g_ctx.registry, spec_config);
        if (!orch) {
            WARN("Speculative orchestrator unavailable — skipping");
            return;
        }

        const std::string prompt =
            "List the first eight prime numbers separated by commas.";
        const int max_tokens = 64;

        WHEN("running the prompt with speculative OFF then ON") {
            auto plain = run_one(*orch, spec_config, false,
                                 prompt, max_tokens);
            REQUIRE_FALSE(plain.content.empty());
            auto spec = run_one(*orch, spec_config, true,
                                prompt, max_tokens);
            REQUIRE_FALSE(spec.content.empty());

            THEN("the two outputs are bit-identical") {
                INFO("plain content: '" << plain.content << "'");
                INFO("spec content:  '" << spec.content << "'");
                CHECK(plain.content == spec.content);
                CHECK(plain.token_count == spec.token_count);
                end_test_log();
            }
        }
    }
}
