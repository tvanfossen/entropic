// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_delegation.cpp
 * @brief BDD subsystem test — lead delegates to child and receives result.
 *
 * Validates that delegation produces a response from the model.
 * Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test 4: Delegation ──────────────────────────────────────

SCENARIO("Lead delegates to child and receives result",
         "[model][test4]")
{
    GIVEN("a configured engine with delegation enabled") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test4_delegation");
        auto params = test_gen_params();
        params.max_tokens = 512;
        params.enable_thinking = false;
        auto messages = make_messages(
            "You are a lead engineer. When asked to delegate, "
            "respond with the delegation plan.",
            "Delegate writing a hello world function to the "
            "eng team member");

        WHEN("I send a task that triggers delegation") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);

            THEN("the model produces a delegation response") {
                REQUIRE_FALSE(result.content.empty());
                CHECK(result.finish_reason == "stop");
                end_test_log();
            }
        }
    }
}
