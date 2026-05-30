// SPDX-License-Identifier: Apache-2.0
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

        WHEN("a write_file tool call is checked against the identity") {
            // gh#87 (v2.7.0): this test validates MCPAuthorizationManager
            // (READ identity denied WRITE), NOT tool-call elicitation. It used
            // to rig a per-adapter prompt + parse the emission, but the small
            // default model does not reliably emit a parseable write call
            // under common_chat (it narrates instead). The auth assertion is
            // model-independent, so we check it on a constructed call —
            // deterministic, and decoupled from model tool-calling quirks.
            entropic::ToolCall tc;
            tc.name = "filesystem.write_file";
            tc.arguments["path"] = "output.txt";
            tc.arguments["content"] = "hello world";

            THEN("the tool call is denied by MCP authorization") {
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
