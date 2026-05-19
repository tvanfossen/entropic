// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v219_gemma4_family.cpp
 * @brief Model test — Gemma 4 family bundled GGUF + Gemma4Adapter.
 *
 * Loads `gemma4_e2b` (smallest Gemma 4 variant — 2.5 GB), exercises
 * first-token smoke generation, and observes raw output for a
 * tool-call prompt. The observed format is the empirical anchor that
 * will refine Gemma4Adapter::parse_tool_calls past its current
 * permissive fallback (header documents the open question).
 *
 * Requires: GPU with >= 8 GB VRAM, model on disk
 *           (`entropic download gemma4_e2b`).
 * Run:      ctest -L model
 *
 * @version 2.1.9
 */

#include "v219_family_test_helpers.h"

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

// ── Tool-call observation ──────────────────────────────────

SCENARIO("Gemma 4 raw output is captured for tool-call format inspection",
         "[model][v219][gemma4][toolcall]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e2b GGUF not present — run `entropic download gemma4_e2b`");
    }
    GIVEN("a prompt requesting a tool call") {
        start_test_log("v219_gemma4_toolcall");
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
                // Empirical confirmation (2026-05-14): Gemma 4 E2B emits
                // tagged JSON tool calls — `<tool_call>{"name":"fs.read",
                // "arguments":{"path":"..."}}</tool_call>` — which the
                // permissive parser handles via parse_tagged_tool_calls.
                // cleaned_content is empty when output is tool-call-only;
                // we assert on raw_content + tool_calls instead.
                REQUIRE_FALSE(result.raw_content.empty());
                spdlog::info("Gemma 4 raw output for tool prompt: {}",
                             result.raw_content);
                REQUIRE(result.tool_calls.size() >= 1);
                CHECK(result.tool_calls[0].name == "fs.read");
                CHECK(result.token_count > 0);
                end_test_log();
            }
        }
    }
}
