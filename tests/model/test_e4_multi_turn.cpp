/**
 * @file test_e4_multi_turn.cpp
 * @brief E4: Multi-turn engine calls retain context.
 *
 * Exercises context retention across two sequential AgentEngine::run()
 * calls. The first turn plants a secret word; the second turn asks
 * for it. Validates that the model references the secret word.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── E4: Multi-turn context retention ────────────────────────

SCENARIO("Multi-turn engine calls retain context",
         "[model][engine]")
{
    GIVEN("an engine wired to the live model") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e4_multi_turn_context");
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

        WHEN("two sequential engine.run() calls share context") {
            auto msgs1 = make_messages(
                "You are a helpful assistant. Remember everything.",
                "The secret word is RAINBOW.");
            auto r1 = engine.run(std::move(msgs1));

            // Build turn 2 from turn 1 result
            auto msgs2 = r1;
            Message follow;
            follow.role = "user";
            follow.content = "What was the secret word?";
            msgs2.push_back(std::move(follow));
            auto r2 = engine.run(std::move(msgs2));

            THEN("turn 2 references the secret word") {
                auto& last = r2.back();
                CHECK(last.content.find("RAINBOW")
                      != std::string::npos);
                end_test_log();
            }
        }
    }
}
