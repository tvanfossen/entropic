/**
 * @file test_gpu_profile.cpp
 * @brief BDD subsystem test — generation succeeds with non-default GPU profile.
 *
 * Validates that generation produces coherent output under GPU profile.
 * Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test 9: GPU Profile / Constrained Generation ────────────

SCENARIO("Generation succeeds with non-default GPU profile",
         "[model][test9]")
{
    GIVEN("a configured engine with the primary model loaded") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test9_gpu_profile");
        auto params = test_gen_params();
        params.max_tokens = 64;
        auto messages = make_messages(
            "You are a helpful assistant.", "Say hello.");

        WHEN("I send a simple prompt") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);

            THEN("the response is coherent") {
                REQUIRE_FALSE(result.content.empty());
                CHECK(result.finish_reason == "stop");
                end_test_log();
            }
        }
    }
}
