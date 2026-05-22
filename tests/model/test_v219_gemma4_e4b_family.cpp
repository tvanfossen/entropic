// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v219_gemma4_e4b_family.cpp
 * @brief Model test — Gemma 4 E4B bundled GGUF + Gemma4Adapter.
 *
 * Loads `gemma4_e4b` (Gemma 4 E4B instruct, ~4.5 GB Q8_0), the
 * mid-class Gemma 4 alternative. Exercises first-token smoke
 * generation and — under the REAL production prompt (constitution +
 * identity + the adapter's own `format_tools`) — guarantees that the
 * tool-call format the model emits is extracted by Gemma4Adapter
 * (gh#69 / gh#71-phase-2).
 *
 * The v2.1.9 version of the toolcall scenario rigged the prompt (spelled
 * out the `<tool_call>{json}` tag in the system message) and asserted
 * `name == "fs.read"` — template-copying, not emission. It missed that
 * Gemma 4 actually emits the `<|im_start|>tool_call` channel form
 * (gh#69). This version drives the adapter's real format_tools and
 * asserts the adapter never misses a real emission.
 *
 * Requires: GPU with >= 8 GB VRAM, model on disk
 *           (`entropic download gemma4_e4b`).
 * Run:      ctest -L model   (release gate — must PASS, not SKIP, when
 *           the GGUF is present)
 *
 * @version 2.3.8
 */

#include "v219_family_test_helpers.h"
#include "production_emission_helpers.h"

namespace { constexpr char K_GEMMA4_E4B[] = "gemma4_e4b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_GEMMA4_E4B>)

// ── First-token smoke ──────────────────────────────────────

SCENARIO("Gemma 4 E4B GGUF loads and generates first token",
         "[model][v219][gemma4][e4b][smoke]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e4b GGUF not present — run `entropic download gemma4_e4b`");
    }
    GIVEN("an engine with gemma4_e4b loaded via Gemma4Adapter") {
        start_test_log("v219_gemma4_e4b_smoke");
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

// ── Real-emission tool-call guarantee (gh#69 / gh#71-phase-2) ──

SCENARIO("Gemma 4 E4B tool calls parse under the production prompt",
         "[model][v219][gemma4][e4b][toolcall]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e4b GGUF not present — run `entropic download gemma4_e4b`");
    }
    GIVEN("the adapter's own format_tools over the production system prompt") {
        start_test_log("v219_gemma4_e4b_toolcall");
        auto* adapter = g_ctx.orchestrator->get_adapter(g_ctx.default_tier);
        REQUIRE(adapter != nullptr);

        // Native tool-call markers the adapter claims to handle: the
        // gh#69 channel header plus the asymmetric / plain tagged forms.
        const std::vector<std::string> markers = {
            "<|im_start|>tool_call", "<|tool_call", "<tool_call>"};

        WHEN("a battery of tool-directed prompts runs through the live model") {
            // Single THEN: Catch2 re-runs WHEN once per leaf section, and
            // each battery run is several live generations — keep it to one.
            auto outcome = run_emission_battery(
                adapter, production_base(), standard_tool_jsons(),
                standard_tool_battery(), markers);

            THEN("the adapter parses every named emission, well-formed, no leak") {
                // gh#69 failure condition: model emits a named call,
                // adapter returns zero and the loop spirals. Every NAMED
                // emission MUST parse.
                REQUIRE_FALSE(outcome.named_missed);
                // Production path elicits AND parses a real call for
                // EVERY directed prompt — the "completely functioning"
                // guarantee. E4B (mid-class) is capable enough to hit this.
                REQUIRE(outcome.parsed == outcome.total);
                // Well-formed names, no native markup left in cleaned_content.
                REQUIRE_FALSE(outcome.malformed);
                REQUIRE_FALSE(outcome.leaked);
                end_test_log();
            }
        }
    }
}
