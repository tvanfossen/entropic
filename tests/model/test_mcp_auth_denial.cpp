// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_mcp_auth_denial.cpp
 * @brief BDD subsystem test — identity with READ-only keys is denied WRITE.
 *
 * Validates MCP authorization denies WRITE access for READ-only identities.
 * Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test 10: MCP Authorization Denial ───────────────────────

SCENARIO("Identity with READ-only keys is denied WRITE access",
         "[model][test10]")
{
    GIVEN("an identity with READ-only filesystem keys") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test10_mcp_auth_denial");
        MCPAuthorizationManager auth;
        auth.register_identity("reader");
        auth.grant("reader", "filesystem.*", MCPAccessLevel::READ);

        WHEN("the model generates a write tool call") {
            // Generate a tool call using the same prompt as test 2
            auto params = test_gen_params();
            params.max_tokens = 512;
            auto messages = make_messages(
                "You are a helpful assistant with filesystem tools.\n\n"
                "# Tools\n\n"
                "Here are the available tools:\n"
                "<tools>\n"
                "[{\"type\":\"function\",\"function\":{\"name\":"
                "\"filesystem.write_file\",\"description\":"
                "\"Write to a file\",\"parameters\":{\"type\":"
                "\"object\",\"properties\":{\"path\":{\"type\":"
                "\"string\"},\"content\":{\"type\":\"string\"}},"
                "\"required\":[\"path\",\"content\"]}}}]\n"
                "</tools>\n\n"
                "For each function call, return within "
                "<tool_call></tool_call> XML tags:\n"
                "<tool_call>\n"
                "<function=example_function>\n"
                "<parameter=param_name>value</parameter>\n"
                "</function>\n"
                "</tool_call>",
                "Write 'hello world' to output.txt");
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);

            // Parse the tool call from model output
            auto* adapter = g_ctx.orchestrator->get_adapter(
                g_ctx.default_tier);
            REQUIRE(adapter != nullptr);
            auto raw = result.raw_content.empty()
                ? result.content : result.raw_content;
            auto parsed = adapter->parse_tool_calls(raw);

            THEN("the tool call is denied by MCP authorization") {
                REQUIRE(parsed.tool_calls.size() >= 1);
                auto& tc = parsed.tool_calls[0];

                // READ identity cannot use WRITE tools
                bool allowed = auth.check_access(
                    "reader", tc.name, MCPAccessLevel::WRITE);
                CHECK_FALSE(allowed);

                // But READ access is granted
                bool read_ok = auth.check_access(
                    "reader", tc.name, MCPAccessLevel::READ);
                CHECK(read_ok);
                end_test_log();
            }
        }
    }
}
