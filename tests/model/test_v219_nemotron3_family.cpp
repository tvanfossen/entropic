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

// gh#87 (v2.7.0): the per-family adapter tool-call scenario was retired with
// the per-family adapters — common_chat now owns tool-call render+parse, and
// per-family common_chat coverage lives in test_gh87_verify_* /
// test_gh87_orchestrator_cutover. The smoke scenario above still gates GGUF
// load + first-token generation.
