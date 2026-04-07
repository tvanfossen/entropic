/**
 * @file test_e8_mcp_auth.cpp
 * @brief E8: MCP authorization denial propagates through engine.
 *
 * Exercises the MCP auth denial path through AgentEngine::run(). A
 * mock tool executor denies all write operations. Validates that the
 * denial is processed and the engine completes with an assistant
 * response.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── E8: MCP auth denial through engine ──────────────────────

SCENARIO("MCP authorization denial propagates through engine",
         "[model][engine]")
{
    GIVEN("an engine with auth-denying tool executor") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e8_mcp_auth_denial");
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
        tei.process_tool_calls = auth_deny_tool_exec;
        tei.user_data = &state;
        engine.set_tool_executor(tei);

        WHEN("engine runs a write request with denial executor") {
            auto messages = make_messages(
                "You are a helpful assistant with filesystem tools.\n\n"
                "# Tools\n\n<tools>\n"
                "[{\"type\":\"function\",\"function\":{\"name\":"
                "\"filesystem.write_file\",\"description\":"
                "\"Write to a file\",\"parameters\":{\"type\":"
                "\"object\",\"properties\":{\"path\":{\"type\":"
                "\"string\"},\"content\":{\"type\":\"string\"}},"
                "\"required\":[\"path\",\"content\"]}}}]\n"
                "</tools>\n\n"
                "For each function call, return within "
                "<tool_call></tool_call> XML tags:\n"
                "<tool_call>\n<function=example_function>\n"
                "<parameter=param_name>value</parameter>\n"
                "</function>\n</tool_call>",
                "Write 'hello world' to output.txt");
            auto result = engine.run(std::move(messages));

            THEN("tool executor was invoked and engine completed with assistant response") {
                CHECK(state.tool_exec_count >= 1);
                REQUIRE(result.size() >= 3);
                CHECK(result.back().role == "assistant");
                end_test_log();
            }
        }
    }
}
