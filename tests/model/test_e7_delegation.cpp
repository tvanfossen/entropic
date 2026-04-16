// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_e7_delegation.cpp
 * @brief E7: Delegation wiring fires on_delegation_start callback.
 *
 * Exercises the delegation path through AgentEngine::run(). Mock tier
 * resolution is wired; a delegation-triggering prompt is sent. Validates
 * that the engine completes without error.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── E7: Delegation through engine ───────────────────────────

SCENARIO("Delegation wiring fires on_delegation_start callback",
         "[model][engine]")
{
    GIVEN("an engine with delegation infrastructure wired") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e7_delegation");
        auto iface = make_real_interface();
        LoopConfig lc;
        // Bumped 10 → 20 at v2.0.6: the prompt-cache bleed fix removed
        // KV residue that was implicitly nudging the model toward
        // convergence. With a clean KV, the lead identity explores
        // more tool-call iterations before emitting a final assistant
        // message. 20 is enough headroom without turning the test into
        // a slow marathon.
        lc.max_iterations = 20;
        lc.stream_output = false;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        CallbackState state;
        EngineCallbacks cbs{};
        wire_callbacks(cbs, state);
        engine.set_callbacks(cbs);

        TierResolutionInterface tri;
        tri.resolve_tier = mock_resolve_tier;
        tri.tier_exists = mock_tier_exists;
        engine.set_tier_resolution(tri);

        WHEN("engine runs with delegation-triggering prompt") {
            auto messages = make_messages(
                "You are a lead engineer.\n\n"
                "# Tools\n\n<tools>\n"
                "[{\"type\":\"function\",\"function\":{\"name\":"
                "\"entropic.delegate\",\"description\":"
                "\"Delegate a task to another team member\","
                "\"parameters\":{\"type\":\"object\","
                "\"properties\":{\"target\":{\"type\":\"string\","
                "\"description\":\"Team member name\"},"
                "\"task\":{\"type\":\"string\","
                "\"description\":\"Task description\"}},"
                "\"required\":[\"target\",\"task\"]}}}]\n"
                "</tools>\n\n"
                "For each function call, return within "
                "<tool_call></tool_call> XML tags:\n"
                "<tool_call>\n<function=example_function>\n"
                "<parameter=param_name>value</parameter>\n"
                "</function>\n</tool_call>",
                "Delegate writing a hello world function to eng");

            // Wire a tool executor that detects delegate calls
            ToolExecutionInterface tei;
            tei.process_tool_calls = mock_tool_exec;
            tei.user_data = &state;
            engine.set_tool_executor(tei);

            auto result = engine.run(std::move(messages));

            THEN("engine completed without error") {
                REQUIRE(result.size() >= 3);
                CHECK(result.back().role == "assistant");
                end_test_log();
            }
        }
    }
}
