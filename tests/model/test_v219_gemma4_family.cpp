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

// gh#87 (v2.7.0): the per-family adapter tool-call scenario was retired with
// the per-family adapters — common_chat now owns tool-call render+parse, and
// per-family common_chat coverage lives in test_gh87_verify_* /
// test_gh87_orchestrator_cutover. The smoke scenario above still gates GGUF
// load + first-token generation.
