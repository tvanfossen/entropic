// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_e1_single_turn.cpp
 * @brief E1: Single-turn engine loop produces coherent response.
 *
 * Exercises a single AgentEngine::run() call with a simple math
 * question. Validates that the model produces a correct answer
 * through the full engine loop.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── E1: Single-turn engine loop ─────────────────────────────

SCENARIO("Single-turn engine loop produces coherent response",
         "[model][engine]")
{
    GIVEN("an engine wired to the live model") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e1_single_turn_engine");
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

        WHEN("engine runs a simple math question") {
            auto messages = make_messages(
                "You are a helpful assistant.", "What is 2+2?");
            auto result = engine.run(std::move(messages));

            THEN("the response contains the answer") {
                REQUIRE(result.size() >= 3);
                auto& last = result.back();
                REQUIRE(last.role == "assistant");
                CHECK(last.content.find("4") != std::string::npos);
                end_test_log();
            }
        }
    }
}
