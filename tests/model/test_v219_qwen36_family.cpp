// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v219_qwen36_family.cpp
 * @brief Model test — Qwen 3.6 family bundled GGUF + Qwen36Adapter.
 *
 * Loads `qwen3_6_a3b` (Qwen3.6-35B-A3B), exercises first-token smoke
 * generation, and confirms that an explicit tool-call instruction
 * yields content the Qwen36Adapter can extract. Developer-run; SKIPs
 * cleanly if the GGUF isn't downloaded.
 *
 * Requires: GPU with >= 16 GB VRAM, model on disk
 *           (`entropic download qwen3_6_a3b`).
 * Run:      ctest -L model
 *
 * @version 2.1.9
 */

#include "v219_family_test_helpers.h"

namespace { constexpr char K_QWEN36[] = "qwen3_6_a3b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_QWEN36>)

// ── First-token smoke ──────────────────────────────────────

SCENARIO("Qwen 3.6 family GGUF loads and generates first token",
         "[model][v219][qwen36][smoke]")
{
    if (!g_ctx.initialized) {
        SKIP("qwen3_6_a3b GGUF not present — run `entropic download qwen3_6_a3b`");
    }
    GIVEN("an engine with qwen3_6_a3b loaded via Qwen36Adapter") {
        start_test_log("v219_qwen36_smoke");
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
