// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_context_retention.cpp
 * @brief BDD subsystem test — multi-turn context retention.
 *
 * Validates that context is maintained across sequential generation
 * calls. Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

SCENARIO("Multi-turn context is retained across generations",
         "[model][test6]")
{
    GIVEN("a configured engine with conversation context") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test6_context_retention");
        auto params = test_gen_params();
        params.max_tokens = 128;
        params.enable_thinking = false;

        WHEN("I send multiple turns building context") {
            auto msgs = make_messages(
                "You are a helpful assistant. The secret word "
                "is RAINBOW. Remember it.",
                "What is the secret word I told you?");
            auto r1 = g_ctx.orchestrator->generate(
                msgs, params, g_ctx.default_tier);

            // Add assistant response and follow up
            Message asst;
            asst.role = "assistant";
            asst.content = r1.content;
            msgs.push_back(asst);
            Message follow;
            follow.role = "user";
            follow.content = "Say it again.";
            msgs.push_back(follow);
            auto r2 = g_ctx.orchestrator->generate(
                msgs, params, g_ctx.default_tier);

            THEN("the model maintains context across turns") {
                CHECK(r2.content.find("RAINBOW")
                      != std::string::npos);
                end_test_log();
            }
        }
    }
}
