// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v219_qwen35_family.cpp
 * @brief Model test — Qwen 3.5 family (qwen35 arch) bundled GGUF.
 *
 * Loads `qwen3_5_0_8b` (Qwen3.5-0.8B Q8_0, ~0.8 GB, GGUF arch qwen35 —
 * hybrid attention + gated-delta-net recurrent layers) and exercises
 * first-token smoke generation. Developer-run; SKIPs cleanly if the GGUF
 * isn't downloaded.
 *
 * v2.14.6: was test_v219_qwen36_family.cpp on `qwen3_6_a3b`
 * (Qwen3.6-35B-A3B, arch qwen35moe), which left the pared model set. The
 * 0.8B is the on-disk member of the same qwen35 adapter family; it is a
 * dense-FFN model, so this no longer covers the MoE variant of the arch.
 *
 * Requires: GPU, model on disk (`entropic download qwen3_5_0_8b`).
 * Run:      ctest -L model
 *
 * @version 2.1.9
 */

#include "v219_family_test_helpers.h"

namespace { constexpr char K_QWEN35[] = "qwen3_5_0_8b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_QWEN35>)

// ── First-token smoke ──────────────────────────────────────

SCENARIO("Qwen 3.5 family GGUF loads and generates first token",
         "[model][v219][qwen35][smoke]")
{
    if (!g_ctx.initialized) {
        SKIP(skip_reason_or_default("qwen3_5_0_8b"));
    }
    GIVEN("an engine with qwen3_5_0_8b loaded") {
        start_test_log("v219_qwen35_smoke");
        auto params = test_gen_params();
        auto messages = make_messages(
            "You are a helpful assistant.", "Say hello in five words.");
        WHEN("a short prompt is sent") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);
            THEN("the engine produces non-empty content") {
                REQUIRE_FALSE(result.content.empty());
                CHECK(result.token_count > 0);
                CHECK(result.throughput_tok_s > 0.0);
                end_test_log();
            }
        }
    }
}

// gh#87 (v2.7.0): the per-family adapter tool-call scenario was retired with
// the per-family adapters — common_chat now owns tool-call render+parse, and
// per-family common_chat coverage lives in test_gh87_verify_* /
// test_gh87_orchestrator_cutover. The smoke scenario above still gates GGUF
// load + first-token generation.
