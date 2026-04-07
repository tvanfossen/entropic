/**
 * @file test_e0_true_e2e.cpp
 * @brief E0: True end-to-end — real prompts, real engine, real model.
 *
 * Exercises the full prompt stack loaded from bundled config through
 * AgentEngine::run() with a live model. Validates state transitions
 * and tier selection with the real identity system prompt.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

SCENARIO("True end-to-end: real prompts, real engine, real model",
         "[model][engine][e2e]")
{
    GIVEN("the full prompt stack loaded from bundled config") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e0_true_end_to_end");
        auto sys_prompt = assemble_system_prompt(g_ctx.default_tier);
        REQUIRE_FALSE(sys_prompt.empty());

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

        WHEN("engine runs with real identity system prompt") {
            auto messages = make_messages(sys_prompt, "Hello");
            auto result = engine.run(std::move(messages));

            THEN("engine completes correctly with real identity") {
                REQUIRE(result.size() >= 3);
                auto& last = result.back();
                CHECK(last.role == "assistant");
                CHECK_FALSE(last.content.empty());

                auto has = [&](AgentState s) {
                    int v = static_cast<int>(s);
                    return std::find(state.states.begin(),
                        state.states.end(), v) != state.states.end();
                };
                CHECK(has(AgentState::PLANNING));
                CHECK(has(AgentState::COMPLETE));
                CHECK_FALSE(state.tier.empty());
                end_test_log();
            }
        }
    }
}
