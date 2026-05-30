// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v219_gemma4_e4b_q4_family.cpp
 * @brief Cross-quant tool-call consistency: E4B at UD-Q4_K_XL (v2.3.8, gh#62).
 *
 * Sibling of test_v219_gemma4_e4b_family.cpp. Loads the 4-bit variant
 * `gemma4_e4b_q4` (gemma-4-E4B-it-UD-Q4_K_XL, ~4.77 GB) and runs the
 * exact same battery under the production system prompt. The intent is
 * to validate that the Gemma4Adapter's tool-calling guarantees hold
 * across quants of the same family/size — same adapter, same prompt,
 * same battery, different quant. A regression here would catch a future
 * quant flooring out below tool-call capability, which the Q8_0 test
 * alone could not detect.
 *
 * Requires: GPU with >= 6 GB VRAM, model on disk
 *           (`entropic download gemma4_e4b_q4`).
 * Run:      ctest -L model   (release gate — must PASS, not SKIP, when
 *           the GGUF is present)
 *
 * @version 2.3.8
 */

#include "v219_family_test_helpers.h"

namespace { constexpr char K_GEMMA4_E4B_Q4[] = "gemma4_e4b_q4"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_GEMMA4_E4B_Q4>)

// ── First-token smoke ──────────────────────────────────────

SCENARIO("Gemma 4 E4B (Q4) GGUF loads and generates first token",
         "[model][v219][gemma4][e4b][q4][smoke]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e4b_q4 GGUF not present — "
             "run `entropic download gemma4_e4b_q4`");
    }
    GIVEN("an engine with gemma4_e4b_q4 loaded via Gemma4Adapter") {
        start_test_log("v219_gemma4_e4b_q4_smoke");
        auto params = test_gen_params();
        auto messages = make_messages(
            "You are a helpful assistant.", "Say hello in five words.");
        WHEN("a short prompt is sent") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);
            THEN("the engine produces non-empty content") {
                REQUIRE_FALSE(result.content.empty());
                CHECK(result.token_count > 0);
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
