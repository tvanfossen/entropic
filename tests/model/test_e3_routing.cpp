// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_e3_routing.cpp
 * @brief E3: Routing fires on_tier_selected callback.
 *
 * Exercises the routing path through AgentEngine::run(). Validates
 * that on_tier_selected fires with a non-empty tier name when the
 * engine processes a programming task.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── E3: Routing with callback verification ──────────────────

SCENARIO("Routing fires on_tier_selected callback",
         "[model][engine]")
{
    GIVEN("an engine with routing callback wired") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e3_routing_callback");
        auto iface = make_real_interface();
        LoopConfig lc;
        lc.max_iterations = 3;
        lc.stream_output = false;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        CallbackState state;
        EngineCallbacks cbs{};
        wire_callbacks(cbs, state);
        engine.set_callbacks(cbs);

        WHEN("engine routes a programming task") {
            auto messages = make_messages(
                "", "Write a Python function that sorts a list");
            auto result = engine.run(std::move(messages));

            THEN("on_tier_selected fired with a tier name") {
                REQUIRE_FALSE(state.tier.empty());
                CHECK(result.size() >= 3);
                end_test_log();
            }
        }
    }
}
