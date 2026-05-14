// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_v219_gemma4_e4b_family.cpp
 * @brief Model test — Gemma 4 E4B bundled GGUF + Gemma4Adapter.
 *
 * Loads `gemma4_e4b` (Gemma 4 E4B instruct, ~4.5 GB Q8_0), the
 * mid-class Gemma 4 alternative. Exercises first-token smoke
 * generation and captures raw output for tool-call format
 * inspection. Same adapter as the E2B / A4B siblings — this test
 * covers validation criterion #6 for the E4B GGUF specifically.
 *
 * Requires: GPU with >= 8 GB VRAM, model on disk
 *           (`entropic download gemma4_e4b`).
 * Run:      ctest -L model
 *
 * @version 2.1.9
 */

#include "v219_family_test_helpers.h"

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

// ── Tool-call observation ──────────────────────────────────

SCENARIO("Gemma 4 E4B raw output captured for tool-call format inspection",
         "[model][v219][gemma4][e4b][toolcall]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e4b GGUF not present — run `entropic download gemma4_e4b`");
    }
    GIVEN("a prompt requesting a tool call") {
        start_test_log("v219_gemma4_e4b_toolcall");
        auto params = test_gen_params();
        params.max_tokens = 256;
        auto messages = make_messages(
            "You may call tools using the format <tool_call>"
            "{\"name\":\"tool.name\",\"arguments\":{...}}</tool_call>.",
            "Call a tool named fs.read with parameter path set to "
            "/etc/hostname. Emit only the tool call.");
        WHEN("the model produces output") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);
            THEN("raw content is logged for empirical refinement") {
                REQUIRE_FALSE(result.content.empty());
                spdlog::info("Gemma 4 E4B raw output: {}", result.raw_content);
                CHECK(result.token_count > 0);
                end_test_log();
            }
        }
    }
}
