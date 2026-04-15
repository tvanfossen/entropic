// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_e5_compaction.cpp
 * @brief E5: Compaction fires when context exceeds threshold.
 *
 * Exercises the compaction path through AgentEngine::run(). Context
 * is padded with 50 user/assistant pairs to exceed the low threshold
 * (0.3). Validates that on_compaction fires and the engine still
 * produces a response.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── E5: Compaction trigger ──────────────────────────────────

SCENARIO("Compaction fires when context exceeds threshold",
         "[model][engine]")
{
    GIVEN("an engine with low compaction threshold and padded context") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e5_compaction_trigger");
        auto iface = make_real_interface();
        LoopConfig lc;
        lc.max_iterations = 3;
        lc.stream_output = false;
        CompactionConfig cc;
        cc.threshold_percent = 0.3f;
        cc.preserve_recent_turns = 1;
        AgentEngine engine(iface, lc, cc);

        CallbackState state;
        EngineCallbacks cbs{};
        wire_callbacks(cbs, state);
        engine.set_callbacks(cbs);

        WHEN("engine runs with context padded past threshold") {
            std::vector<Message> messages;
            Message sys;
            sys.role = "system";
            sys.content = "You are a helpful assistant.";
            messages.push_back(std::move(sys));

            // Pad with 50 user/assistant pairs (~200 tokens each)
            std::string filler =
                "The solar system contains eight planets orbiting "
                "the Sun. Mercury is closest, then Venus, Earth, "
                "Mars, Jupiter, Saturn, Uranus, and Neptune. Each "
                "has unique characteristics worth studying.";
            for (int i = 0; i < 50; ++i) {
                Message u;
                u.role = "user";
                u.content = "Tell me fact " + std::to_string(i + 1)
                    + ". " + filler;
                Message a;
                a.role = "assistant";
                a.content = "Fact " + std::to_string(i + 1)
                    + ": " + filler;
                messages.push_back(std::move(u));
                messages.push_back(std::move(a));
            }

            Message final_q;
            final_q.role = "user";
            final_q.content = "Forget all that. What is 1+1? "
                "Answer with just the number.";
            messages.push_back(std::move(final_q));

            auto result = engine.run(std::move(messages));

            THEN("compaction fired and engine still produced a response") {
                CHECK(state.compaction_fired);
                REQUIRE(result.size() >= 2);
                CHECK(result.back().role == "assistant");
                end_test_log();
            }
        }
    }
}
