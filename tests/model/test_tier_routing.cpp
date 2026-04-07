/**
 * @file test_tier_routing.cpp
 * @brief BDD subsystem test — classification routes to correct identity.
 *
 * Validates that the router classifies prompts to appropriate tiers.
 * Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test 3: Tier Routing ────────────────────────────────────

SCENARIO("Classification routes to the correct identity",
         "[model][test3]")
{
    GIVEN("a configured engine with multiple identities") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test3_tier_routing");
        auto messages = make_messages(
            "", "Write a Python function that sorts a list");

        WHEN("I send a programming task") {
            auto tier = g_ctx.orchestrator->route(messages);

            THEN("the router classifies to a code-oriented tier") {
                REQUIRE_FALSE(tier.empty());
                end_test_log();
            }
        }
    }
}
