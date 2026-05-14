// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_v219_nemotron3_family.cpp
 * @brief Model test — Nemotron 3 family bundled GGUF + Nemotron3Adapter.
 *
 * Loads `nemotron3_nano_4b` (hybrid Mamba-Transformer, GGUF arch
 * `nemotron_h`), exercises first-token smoke generation, and
 * confirms that the qwen3_coder XML tool-call format is extracted
 * by Nemotron3Adapter. Reasoning traces (`<think>...</think>`) are
 * expected to be stripped from `cleaned_content` by the base class.
 *
 * Requires: GPU with >= 6 GB VRAM, model on disk
 *           (`entropic download nemotron3_nano_4b`).
 * Run:      ctest -L model
 *
 * @version 2.1.9
 */

#include "v219_family_test_helpers.h"

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

// ── Tool-call fixture (qwen3_coder XML) ────────────────────

SCENARIO("Nemotron 3 emits parseable XML tool calls",
         "[model][v219][nemotron3][toolcall]")
{
    if (!g_ctx.initialized) {
        SKIP("nemotron3_nano_4b GGUF not present — "
             "run `entropic download nemotron3_nano_4b`");
    }
    GIVEN("a prompt instructing the model to emit an XML tool call") {
        start_test_log("v219_nemotron3_toolcall");
        auto params = test_gen_params();
        params.max_tokens = 256;
        auto messages = make_messages(
            "You may call tools. Emit calls as XML: "
            "<tool_call><function=name><parameter=key>value</parameter>"
            "</function></tool_call>.",
            "Call fs.read with path /etc/hostname. Emit only the call.");
        WHEN("the model is asked to emit a tool call") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);
            THEN("Nemotron3Adapter parses the XML call and strips think blocks") {
                // Empirical confirmation (2026-05-14): Nemotron 3 emits
                // <think>...</think> reasoning then a qwen3_coder XML
                // tool call, e.g. <tool_call><function=read>...
                // </function></tool_call>. The model picks a function
                // name from the prompt — may shorten "fs.read" to "read".
                // We assert the parser extracted SOMETHING and the
                // think block did not leak into cleaned content.
                REQUIRE_FALSE(result.raw_content.empty());
                REQUIRE(result.tool_calls.size() >= 1);
                CHECK(result.content.find("<think>") == std::string::npos);
                CHECK(result.content.find("</think>") == std::string::npos);
                CHECK(result.token_count > 0);
                end_test_log();
            }
        }
    }
}
