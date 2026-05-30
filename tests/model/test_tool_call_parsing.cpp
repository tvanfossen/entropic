// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_tool_call_parsing.cpp
 * @brief BDD subsystem test — tool call parsing from model output.
 *
 * Validates that model output is correctly parsed into tool calls.
 * Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test 2: Tool Call Parsing ───────────────────────────────

SCENARIO("Model output is correctly parsed into tool calls",
         "[model][test2]")
{
    GIVEN("a filesystem tool supplied via params.tools (common_chat)") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test2_tool_call_parsing");
        // gh#87 (v2.7.0): tools flow via params.tools and common_chat renders
        // them in the model's native format + parses the emission. No rigged
        // per-adapter prompt, no direct adapter->parse_tool_calls call.
        auto params = test_gen_params();
        params.max_tokens = 512;
        params.tools =
            R"([{"name":"filesystem.read_file",)"
            R"("description":"Read a file from disk.",)"
            R"("inputSchema":{"type":"object","properties":)"
            R"({"path":{"type":"string"}},"required":["path"]}}])";
        auto messages = make_messages(
            "You are a helpful assistant with filesystem tools.",
            "Use the read_file tool to read the file test.txt. "
            "Emit only the tool call.");

        WHEN("the orchestrator generates with the tool staged") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);

            THEN("common_chat extracts the tool call") {
                INFO("raw=[" << result.raw_content << "] calls="
                     << result.tool_calls.size());
                REQUIRE(result.tool_calls.size() >= 1);
                auto& tc = result.tool_calls[0];
                CHECK(tc.name.find("read_file") != std::string::npos);
                CHECK(tc.arguments.count("path") > 0);
                end_test_log();
            }
        }
    }
}
