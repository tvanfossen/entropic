/**
 * @file test_e6_state_transitions.cpp
 * @brief E6: Engine state transitions include PLANNING and EXECUTING.
 *
 * Exercises the state machine through AgentEngine::run(). Validates
 * that the on_state_change callback records PLANNING, EXECUTING, and
 * COMPLETE in the state sequence.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── E6: State transition sequence ───────────────────────────

SCENARIO("Engine state transitions include PLANNING and EXECUTING",
         "[model][engine]")
{
    GIVEN("an engine with state change callback") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e6_state_transitions");
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

        WHEN("engine runs a simple prompt") {
            auto messages = make_messages(
                "You are a helpful assistant.", "Say hello.");
            engine.run(std::move(messages));

            THEN("state sequence includes PLANNING and EXECUTING") {
                auto has = [&](AgentState s) {
                    int v = static_cast<int>(s);
                    return std::find(state.states.begin(),
                        state.states.end(), v) != state.states.end();
                };
                CHECK(has(AgentState::PLANNING));
                CHECK(has(AgentState::EXECUTING));
                CHECK(has(AgentState::COMPLETE));
                end_test_log();
            }
        }
    }
}
