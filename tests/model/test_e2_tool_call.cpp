// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_e2_tool_call.cpp
 * @brief E2: Tool call cycle — generate, parse, execute, regenerate.
 *
 * Exercises the full tool execution cycle through AgentEngine::run().
 * A mock tool executor returns scripted content; validates that the
 * engine invokes the executor and produces a final assistant response.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── E2: Tool call cycle through engine ──────────────────────

SCENARIO("Tool call cycle: generate, parse, execute, regenerate",
         "[model][engine]")
{
    GIVEN("an engine with tool executor wired") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e2_tool_call_cycle");
        auto iface = make_real_interface();
        LoopConfig lc;
        lc.max_iterations = 5;
        lc.auto_approve_tools = true;
        lc.stream_output = false;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        CallbackState state;
        EngineCallbacks cbs{};
        wire_callbacks(cbs, state);
        engine.set_callbacks(cbs);

        ToolExecutionInterface tei;
        tei.process_tool_calls = mock_tool_exec;
        tei.user_data = &state;
        engine.set_tool_executor(tei);

        WHEN("engine runs a file-read request with tools") {
            // gh#87 (v2.7.0): tools are no longer rigged into the prompt.
            // They flow via the harness get_tool_prompt → params.tools →
            // common_chat, which renders them in the model's native format.
            auto messages = make_messages(
                "You are a helpful assistant with filesystem tools.",
                "Read the file test.txt with the read_file tool and tell "
                "me what it says.");
            auto result = engine.run(std::move(messages));

            THEN("tool executor was invoked and engine produced a final response") {
                CHECK(state.tool_exec_count >= 1);
                REQUIRE(result.size() >= 3);
                CHECK(result.back().role == "assistant");
                end_test_log();
            }
        }
    }
}
