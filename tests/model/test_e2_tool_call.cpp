// SPDX-License-Identifier: LGPL-3.0-or-later
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
            auto messages = make_messages(
                "You are a helpful assistant with filesystem tools.\n\n"
                "# Tools\n\n<tools>\n"
                "[{\"type\":\"function\",\"function\":{\"name\":"
                "\"filesystem.read_file\",\"description\":"
                "\"Read a file\",\"parameters\":{\"type\":\"object\","
                "\"properties\":{\"path\":{\"type\":\"string\"}},"
                "\"required\":[\"path\"]}}}]\n</tools>\n\n"
                "For each function call, return within "
                "<tool_call></tool_call> XML tags:\n"
                "<tool_call>\n<function=example_function>\n"
                "<parameter=param_name>value</parameter>\n"
                "</function>\n</tool_call>",
                "Read the file test.txt and tell me what it says");
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
