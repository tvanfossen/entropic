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
#include "production_emission_helpers.h"

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

// ── Cross-quant tool-call consistency (gh#62) ──────────────

SCENARIO("Gemma 4 E4B (Q4) tool calls parse under the production prompt",
         "[model][v219][gemma4][e4b][q4][toolcall]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e4b_q4 GGUF not present — "
             "run `entropic download gemma4_e4b_q4`");
    }
    GIVEN("the adapter's own format_tools over the production system prompt") {
        start_test_log("v219_gemma4_e4b_q4_toolcall");
        auto* adapter = g_ctx.orchestrator->get_adapter(g_ctx.default_tier);
        REQUIRE(adapter != nullptr);

        // Same native markers as the Q8_0 E4B test: the gh#69 channel
        // header plus the asymmetric / plain tagged forms. Quant does
        // not change the format the adapter is expected to handle.
        const std::vector<std::string> markers = {
            "<|im_start|>tool_call", "<|tool_call", "<tool_call>"};

        WHEN("a battery of tool-directed prompts runs through the live model") {
            auto outcome = run_emission_battery(
                adapter, production_base(), standard_tool_jsons(),
                standard_tool_battery(), markers);

            THEN("the adapter parses every named emission, well-formed, no leak") {
                // Cross-quant consistency: the 4-bit variant MUST hit the
                // same strict guarantee as the Q8_0 sibling — every named
                // emission parses, every directed prompt yields a call,
                // zero leakage. If a future quant degrades below this bar
                // (e.g. starts emitting nameless calls like E2B), this
                // fails loudly instead of silently regressing in production.
                REQUIRE_FALSE(outcome.named_missed);
                REQUIRE(outcome.parsed == outcome.total);
                REQUIRE_FALSE(outcome.malformed);
                REQUIRE_FALSE(outcome.leaked);
                end_test_log();
            }
        }
    }
}
