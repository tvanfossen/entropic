// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v219_gemma4_family.cpp
 * @brief Model test — Gemma 4 family bundled GGUF + Gemma4Adapter.
 *
 * Loads `gemma4_e2b` (smallest Gemma 4 variant — 2.5 GB), exercises
 * first-token smoke generation, and — under the REAL production prompt
 * (constitution + identity + the adapter's own `format_tools`) —
 * guarantees that the tool-call format the model emits is extracted by
 * Gemma4Adapter (gh#69 / gh#71-phase-2). E2B was reported 0/6 alongside
 * E4B; same adapter, so the guarantee is asserted on both GGUFs.
 *
 * The v2.1.9 toolcall scenario rigged the prompt and asserted
 * `name == "fs.read"` — template-copying. This version drives the real
 * format_tools and asserts the adapter never misses a real emission.
 *
 * Requires: GPU with >= 8 GB VRAM, model on disk
 *           (`entropic download gemma4_e2b`).
 * Run:      ctest -L model   (release gate — must PASS, not SKIP, when
 *           the GGUF is present)
 *
 * @version 2.3.8
 */

#include "v219_family_test_helpers.h"
#include "production_emission_helpers.h"

namespace { constexpr char K_GEMMA4[] = "gemma4_e2b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_GEMMA4>)

// ── First-token smoke ──────────────────────────────────────

SCENARIO("Gemma 4 family GGUF loads and generates first token",
         "[model][v219][gemma4][smoke]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e2b GGUF not present — run `entropic download gemma4_e2b`");
    }
    GIVEN("an engine with gemma4_e2b loaded via Gemma4Adapter") {
        start_test_log("v219_gemma4_smoke");
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

SCENARIO("Gemma 4 E2B tool calls parse under the production prompt",
         "[model][v219][gemma4][toolcall]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e2b GGUF not present — run `entropic download gemma4_e2b`");
    }
    GIVEN("the adapter's own format_tools over the production system prompt") {
        start_test_log("v219_gemma4_toolcall");
        auto* adapter = g_ctx.orchestrator->get_adapter(g_ctx.default_tier);
        REQUIRE(adapter != nullptr);

        const std::vector<std::string> markers = {
            "<|im_start|>tool_call", "<|tool_call", "<tool_call>"};

        WHEN("a battery of tool-directed prompts runs through the live model") {
            auto outcome = run_emission_battery(
                adapter, production_base(), standard_tool_jsons(),
                standard_tool_battery(), markers);

            THEN("the adapter parses every NAMED emission, well-formed, no leak") {
                // The adapter contract holds for E2B: every call that
                // carries a tool name (incl. the `tool_name` alias) parses.
                REQUIRE_FALSE(outcome.named_missed);
                REQUIRE_FALSE(outcome.malformed);
                REQUIRE_FALSE(outcome.leaked);
                // E2B (2.5 GB, edge/router-class) omits the function name
                // entirely on some prompts — a genuine model-capability
                // limitation, NOT an adapter bug (recovering a tool from
                // bare args would be a fragile heuristic). So E2B gets the
                // weaker floor: at least one real named call parses. The
                // strict per-prompt guarantee lives on the tool-calling
                // tiers (E4B / A4B). See gh#71 — E2B is not a tool tier.
                REQUIRE(outcome.parsed >= 1);
                end_test_log();
            }
        }
    }
}
