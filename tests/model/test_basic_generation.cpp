// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_basic_generation.cpp
 * @brief BDD subsystem test — basic text generation with live model.
 *
 * Validates that the engine produces coherent text from a simple prompt.
 * Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test 1: Basic Generation ────────────────────────────────

SCENARIO("Engine produces coherent text from a simple prompt",
         "[model][test1]")
{
    GIVEN("a configured engine with the primary model loaded") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test1_basic_generation");
        auto params = test_gen_params();
        auto messages = make_messages(
            "You are a helpful assistant.", "What is 2+2?");

        WHEN("I send a simple math question") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);

            THEN("the response contains the correct answer") {
                REQUIRE_FALSE(result.content.empty());
                REQUIRE(result.content.find("4")
                        != std::string::npos);
                CHECK(result.token_count > 0);
                CHECK(result.finish_reason == "stop");
                CHECK(result.throughput_tok_s > 0.0);
                end_test_log();
            }
        }
    }
}
