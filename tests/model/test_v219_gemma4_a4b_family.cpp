// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v219_gemma4_a4b_family.cpp
 * @brief Model test — Gemma 4 26B-A4B bundled GGUF + Gemma4Adapter.
 *
 * Loads `gemma4_a4b` (Gemma 4 26B-A4B instruct, UD-IQ4_XS ~13.6 GB) —
 * the primary-class alternative used as a validation contrast against
 * the Qwen primary. Exercises first-token smoke generation and
 * captures raw output for tool-call format inspection. Same
 * `Gemma4Adapter` as the E-series; this test covers validation
 * criterion #6 for the A4B variant specifically.
 *
 * Requires: GPU with >= 16 GB VRAM, model on disk
 *           (`entropic download gemma4_a4b`).
 * Run:      ctest -L model
 *
 * @version 2.1.9
 */

#include "v219_family_test_helpers.h"

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

// ── Tool-call observation ──────────────────────────────────

SCENARIO("Gemma 4 A4B raw output captured for tool-call format inspection",
         "[model][v219][gemma4][a4b][toolcall]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_a4b GGUF not present — run `entropic download gemma4_a4b`");
    }
    GIVEN("a prompt requesting a tool call") {
        start_test_log("v219_gemma4_a4b_toolcall");
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
            THEN("Gemma4Adapter extracts the tool call from raw output") {
                REQUIRE_FALSE(result.raw_content.empty());
                spdlog::info("Gemma 4 A4B raw output: {}", result.raw_content);
                // Same contract as E2B/E4B — Gemma 4 family emits tagged
                // JSON tool calls.
                REQUIRE(result.tool_calls.size() >= 1);
                CHECK(result.tool_calls[0].name == "fs.read");
                CHECK(result.token_count > 0);
                end_test_log();
            }
        }
    }
}
