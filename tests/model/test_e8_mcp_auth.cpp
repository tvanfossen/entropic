// SPDX-License-Identifier: Apache-2.0
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
            // gh#87 (v2.7.0): tools flow via params.tools → common_chat, not
            // a rigged prompt. The harness serves a write_file tool def. The
            // "only the tool call" directive is the proven pattern (gh#87
            // verify helpers) for eliciting a clean native call across
            // families — without it a thinking model narrates + emits an
            // empty <tool_call> stub.
            auto messages = make_messages(
                "You are a helpful assistant with filesystem tools.",
                "Use the write_file tool to write 'hello world' to the file "
                "output.txt. Respond with only the tool call.");
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
