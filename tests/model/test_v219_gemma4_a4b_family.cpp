// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v219_gemma4_a4b_family.cpp
 * @brief Model test — Gemma 4 26B-A4B bundled GGUF + Gemma4Adapter.
 *
 * Loads `gemma4_a4b` (Gemma 4 26B-A4B instruct, UD-IQ4_XS ~13.6 GB) —
 * the primary-class alternative used as a validation contrast against
 * the Qwen primary. Exercises first-token smoke generation and — under
 * the REAL production prompt (constitution + identity + the adapter's
 * own `format_tools`) — guarantees tool-call extraction via
 * Gemma4Adapter (gh#69 / gh#71-phase-2). Same `Gemma4Adapter` as the
 * E-series.
 *
 * The v2.1.9 toolcall scenario rigged the prompt and asserted
 * `name == "fs.read"`. This version drives the real format_tools and
 * asserts the adapter never misses a real emission. NOTE: the 26B-A4B
 * GGUF + KV does not fit a 16 GB GPU, so this SKIPs there; it is the
 * release gate for hosts with >= ~20 GB VRAM.
 *
 * Requires: GPU with >= 16 GB VRAM, model on disk
 *           (`entropic download gemma4_a4b`).
 * Run:      ctest -L model
 *
 * @version 2.3.8
 */

#include "v219_family_test_helpers.h"
#include "production_emission_helpers.h"

namespace { constexpr char K_GEMMA4_A4B[] = "gemma4_a4b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_GEMMA4_A4B>)

// ── First-token smoke ──────────────────────────────────────

SCENARIO("Gemma 4 A4B GGUF loads and generates first token",
         "[model][v219][gemma4][a4b][smoke]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_a4b GGUF not present — run `entropic download gemma4_a4b`");
    }
    GIVEN("an engine with gemma4_a4b loaded via Gemma4Adapter") {
        start_test_log("v219_gemma4_a4b_smoke");
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

SCENARIO("Gemma 4 A4B tool calls parse under the production prompt",
         "[model][v219][gemma4][a4b][toolcall]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_a4b GGUF not present — run `entropic download gemma4_a4b`");
    }
    GIVEN("the adapter's own format_tools over the production system prompt") {
        start_test_log("v219_gemma4_a4b_toolcall");
        auto* adapter = g_ctx.orchestrator->get_adapter(g_ctx.default_tier);
        REQUIRE(adapter != nullptr);

        const std::vector<std::string> markers = {
            "<|im_start|>tool_call", "<|tool_call", "<tool_call>"};

        WHEN("a battery of tool-directed prompts runs through the live model") {
            auto outcome = run_emission_battery(
                adapter, production_base(), standard_tool_jsons(),
                standard_tool_battery(), markers);

            THEN("the adapter parses every named emission, well-formed, no leak") {
                REQUIRE_FALSE(outcome.named_missed);
                REQUIRE(outcome.parsed == outcome.total);
                REQUIRE_FALSE(outcome.malformed);
                REQUIRE_FALSE(outcome.leaked);
                end_test_log();
            }
        }
    }
}
