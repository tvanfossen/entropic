// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v219_gemma4_a4b_family.cpp
 * @brief Model test — Gemma 4 26B-A4B bundled GGUF + Gemma4Adapter.
 *
 * Loads `gemma4_a4b_iq2` (Gemma 4 26B-A4B instruct, UD-IQ2_M ~10.0 GB,
 * 9.33 GiB — under the 10 GiB large-model threshold, so it loads at full
 * offload on an 11 GB card; v2.14.6: was `gemma4_a4b_qat`, which left the
 * pared model set) — the MoE representative of the Gemma 4 family. Exercises first-token smoke generation and — under
 * the REAL production prompt (constitution + identity + the adapter's
 * own `format_tools`) — guarantees tool-call extraction via
 * Gemma4Adapter (gh#69 / gh#71-phase-2). Same `Gemma4Adapter` as the
 * E-series.
 *
 * The v2.1.9 toolcall scenario rigged the prompt and asserted
 * `name == "fs.read"`. This version drives the real format_tools and
 * asserts the adapter never misses a real emission.
 *
 * Requires: GPU with >= 11 GB VRAM, model on disk
 *           (`entropic download gemma4_a4b_iq2`).
 * Run:      ctest -L model
 *
 * @version 2.3.8
 */

#include "v219_family_test_helpers.h"

namespace { constexpr char K_GEMMA4_A4B[] = "gemma4_a4b_iq2"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_GEMMA4_A4B>)

// ── First-token smoke ──────────────────────────────────────

SCENARIO("Gemma 4 A4B GGUF loads and generates first token",
         "[model][v219][gemma4][a4b][smoke]")
{
    if (!g_ctx.initialized) {
        SKIP(skip_reason_or_default("gemma4_a4b_iq2"));
    }
    GIVEN("an engine with gemma4_a4b_iq2 loaded via Gemma4Adapter") {
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

// gh#87 (v2.7.0): the per-family adapter tool-call battery was retired with
// the Gemma4Adapter — common_chat now owns tool-call render+parse. Per-family
// common_chat tool-call coverage lives in test_gh87_verify_* and
// test_gh87_orchestrator_cutover (gemma4-a4b). The smoke scenario above still
// gates GGUF load + first-token generation for this quant.
