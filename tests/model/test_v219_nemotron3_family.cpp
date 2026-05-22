// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v219_nemotron3_family.cpp
 * @brief Model test — Nemotron 3 family bundled GGUF + Nemotron3Adapter.
 *
 * Loads `nemotron3_nano_4b` (hybrid Mamba-Transformer, GGUF arch
 * `nemotron_h`), exercises first-token smoke generation, and — under
 * the REAL production prompt (constitution + identity + the adapter's
 * own `format_tools`) — guarantees that whatever tool-call format the
 * model emits is extracted by Nemotron3Adapter (gh#70 / gh#71-phase-2).
 *
 * The v2.1.9 version of the toolcall scenario rigged the prompt (handed
 * the model the XML tag template) and asserted `>= 1` once — it tested
 * template-copying, not emission, and so missed that the model actually
 * emits DSML invoke (gh#70). This version drives the adapter's real
 * format_tools and asserts the adapter never misses a real emission.
 *
 * Requires: GPU with >= 6 GB VRAM, model on disk
 *           (`entropic download nemotron3_nano_4b`).
 * Run:      ctest -L model   (release gate — must PASS, not SKIP, when
 *           the GGUF is present)
 *
 * @version 2.3.8
 */

#include "v219_family_test_helpers.h"
#include "production_emission_helpers.h"

namespace { constexpr char K_NEMOTRON3[] = "nemotron3_nano_4b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_NEMOTRON3>)

// ── First-token smoke ──────────────────────────────────────

SCENARIO("Nemotron 3 family GGUF loads and generates first token",
         "[model][v219][nemotron3][smoke]")
{
    if (!g_ctx.initialized) {
        SKIP("nemotron3_nano_4b GGUF not present — "
             "run `entropic download nemotron3_nano_4b`");
    }
    GIVEN("an engine with nemotron3_nano_4b loaded via Nemotron3Adapter") {
        start_test_log("v219_nemotron3_smoke");
        auto params = test_gen_params();
        auto messages = make_messages(
            "You are a helpful assistant. "
            "Respond concisely without long reasoning traces.",
            "What is 2 + 2?");
        WHEN("a short math prompt is sent") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);
            THEN("the engine produces non-empty content containing the answer") {
                REQUIRE_FALSE(result.content.empty());
                CHECK(result.content.find("4") != std::string::npos);
                CHECK(result.token_count > 0);
                end_test_log();
            }
        }
    }
}

// ── Real-emission tool-call guarantee (gh#70 / gh#71-phase-2) ──

SCENARIO("Nemotron 3 tool calls parse under the production prompt",
         "[model][v219][nemotron3][toolcall]")
{
    if (!g_ctx.initialized) {
        SKIP("nemotron3_nano_4b GGUF not present — "
             "run `entropic download nemotron3_nano_4b`");
    }
    GIVEN("the adapter's own format_tools over the production system prompt") {
        start_test_log("v219_nemotron3_toolcall");
        auto* adapter = g_ctx.orchestrator->get_adapter(g_ctx.default_tier);
        REQUIRE(adapter != nullptr);

        // Native tool-call markers the adapter claims to handle: DSML
        // invoke (primary, gh#70) plus the qwen XML / tagged-JSON
        // backstops. NOT bare `<｜` — that would match BOS spam, which
        // is not a tool call.
        const std::vector<std::string> markers = {
            "<｜DSML｜invoke", "<function=", "<tool_call>"};

        WHEN("a battery of tool-directed prompts runs through the live model") {
            // Single THEN: Catch2 re-runs WHEN once per leaf section, and
            // each battery run is several live generations — keep it to one.
            auto outcome = run_emission_battery(
                adapter, production_base(), standard_tool_jsons(),
                standard_tool_battery(), markers);

            THEN("the adapter parses every named emission, well-formed, no leak") {
                // gh#70 failure condition: model emits a named DSML call,
                // adapter returns zero. Every NAMED emission MUST parse.
                REQUIRE_FALSE(outcome.named_missed);
                // Production path elicits AND parses a real call for
                // EVERY directed prompt — the "completely functioning"
                // guarantee, not just "extracted something once". Nemotron
                // 3 (4B) is capable enough to hit this consistently.
                REQUIRE(outcome.parsed == outcome.total);
                // Well-formed names, no native markup left in cleaned_content.
                REQUIRE_FALSE(outcome.malformed);
                REQUIRE_FALSE(outcome.leaked);
                end_test_log();
            }
        }
    }
}
