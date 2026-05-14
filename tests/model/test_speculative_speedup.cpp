// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_speculative_speedup.cpp
 * @brief Validate the v2.1.11 speculative-decoding speedup gate.
 *
 * The v2.1.11 proposal sets a model-test gate: speculative decoding
 * must deliver ≥1.8× decode throughput speedup on long generations
 * (>500 tokens) against a tokenizer-compatible target/draft pair.
 *
 * Test pair: Qwen3.6-A3B target (GPU, IQ3_XXS) + Qwen3.5-0.8B
 * draft (CPU, Q8_0, `n_gpu_layers=0`). The CPU placement preserves
 * the GPU's full 15.2 GB / 16 GB VRAM budget for the verifier at
 * 128k context.
 *
 * The test runs the same long-form prompt twice — once with
 * speculative OFF, once ON — and asserts the speedup ratio meets
 * the proposal's gate. Acceptance rate is logged for diagnostic
 * value; the proposal's expected range is 0.5–0.8 for a well-paired
 * draft, but the binding contract is the speedup.
 *
 * Requires: GPU with ≥ 16 GB VRAM, both GGUFs present locally,
 *           system idle enough for stable throughput measurement.
 * Run: ctest -L model -R speculative-speedup
 *
 * @version 2.1.11
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

namespace {

constexpr int SPEEDUP_MAX_TOKENS = 600;          // >500 per the gate
constexpr float SPEEDUP_GATE = 1.8f;             // ≥1.8× required

/**
 * @brief Configure orchestrator with speculative on, CPU draft.
 *
 * @return Orchestrator or nullptr when the draft GGUF is missing.
 * @utility
 * @version 2.1.11
 */
std::unique_ptr<ModelOrchestrator> build_speedup_orchestrator(
    const config::BundledModels& registry,
    ParsedConfig& config) {
    auto draft_path = registry.resolve("qwen3_5_0_8b");
    if (!fs::exists(draft_path)) {
        WARN("Qwen3.5-0.8B draft GGUF missing — skipping");
        return nullptr;
    }
    config.inference.speculative.enabled = true;
    config.inference.speculative.n_draft = 16;
    config.inference.speculative.draft.path = draft_path;

    // Shrink primary context for VRAM budget — speedup is the metric
    // we care about; 8192 ctx is plenty for a ~600-token generation.
    // Disable flash attention so the primary supports PARTIAL seq_rm
    // (kernel requirement).
    for (auto& [name, tier] : config.models.tiers) {
        tier.context_length = 8192;
        tier.flash_attn = false;
    }

    if (g_ctx.orchestrator) {
        g_ctx.orchestrator->shutdown();
        g_ctx.orchestrator.reset();
    }

    auto orch = std::make_unique<ModelOrchestrator>();
    if (!orch->initialize(config)) {
        WARN("Speedup orchestrator init failed");
        return nullptr;
    }
    return orch;
}

/**
 * @brief Measure tokens/sec for one generation run.
 *
 * @param orch Orchestrator.
 * @param config Parsed config (speculative flag mutated).
 * @param speculative_on Whether to take the speculative path.
 * @return Pair of (result, tokens_per_second).
 * @utility
 * @version 2.1.11
 */
std::pair<GenerationResult, double> measure_throughput(
    ModelOrchestrator& orch, ParsedConfig& config,
    bool speculative_on) {
    config.inference.speculative.enabled = speculative_on;
    GenerationParams params;
    params.temperature = 0.7f;
    params.top_p = 0.9f;
    params.top_k = 40;
    params.seed = 1337;
    params.max_tokens = SPEEDUP_MAX_TOKENS;
    auto messages = make_messages(
        "You are a verbose technical writer who provides "
        "comprehensive, multi-paragraph answers with full detail.",
        "Explain how speculative decoding works in large language "
        "model inference. Cover: (1) the draft-target relationship, "
        "(2) why it accelerates without changing output distribution, "
        "(3) the role of tokenizer compatibility, (4) tradeoffs "
        "between draft size and acceptance rate, (5) realistic "
        "speedup numbers seen in practice. Be thorough.");
    auto result = orch.generate(messages, params, "");
    double tps = 0.0;
    if (result.generation_time_ms > 0 && result.token_count > 0) {
        tps = static_cast<double>(result.token_count)
              / (result.generation_time_ms / 1000.0);
    }
    return {result, tps};
}

} // anonymous namespace

SCENARIO("Speculative decoding delivers ≥1.8× speedup on long "
         "generations (model-test gate)",
         "[model][speculative][speedup]")
{
    GIVEN("Qwen3.6-A3B target + Qwen3.5-0.8B CPU-resident draft") {
        REQUIRE(g_ctx.initialized);
        start_test_log("speculative_speedup");

        ParsedConfig spec_config = g_ctx.config;
        auto orch = build_speedup_orchestrator(
            g_ctx.registry, spec_config);
        if (!orch) {
            WARN("Speedup test setup failed — skipping");
            return;
        }

        WHEN("running the prompt with speculative OFF then ON") {
            // Warm-up pass to settle GPU clocks and any first-call
            // overhead (the measurement passes below will be the
            // ones used for the ratio).
            ParsedConfig warm = spec_config;
            measure_throughput(*orch, warm, false);

            auto [plain, plain_tps] =
                measure_throughput(*orch, spec_config, false);
            auto [spec, spec_tps] =
                measure_throughput(*orch, spec_config, true);

            REQUIRE_FALSE(plain.content.empty());
            REQUIRE_FALSE(spec.content.empty());
            REQUIRE(plain.token_count >= 500);
            REQUIRE(spec.token_count >= 500);

            const double speedup = spec_tps / plain_tps;
            spdlog::info("[speculative-speedup] plain={} tok/s "
                         "({} tokens), spec={} tok/s ({} tokens), "
                         "speedup={}×",
                         plain_tps, plain.token_count,
                         spec_tps, spec.token_count, speedup);

            THEN("the speculative throughput beats plain by the gate") {
                INFO("plain tok/s: " << plain_tps);
                INFO("speculative tok/s: " << spec_tps);
                INFO("speedup: " << speedup);
                CHECK(speedup >= SPEEDUP_GATE);
                end_test_log();
            }
        }
    }
}
