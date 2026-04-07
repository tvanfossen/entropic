/**
 * @file test_external_mcp.cpp
 * @brief BDD subsystem test — tool call via external MCP server (stdio).
 *
 * Validates that ExternalMCPClient connects to a child process via
 * StdioTransport and executes a tool call that returns the expected
 * echoed text. Uses the mock_mcp_server binary.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk, mock server binary.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

#include <entropic/mcp/external_client.h>
#include <entropic/mcp/transport_stdio.h>

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test: External MCP Tool Call ────────────────────────────

SCENARIO("Tool call executes through external MCP stdio transport",
         "[model][external_mcp]")
{
    GIVEN("a model loaded and external MCP client connected") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test_external_mcp");

#ifdef MOCK_MCP_SERVER_PATH
        std::string server_path = MOCK_MCP_SERVER_PATH;
#else
        SKIP("MOCK_MCP_SERVER_PATH not defined — "
             "mock server binary not available");
        std::string server_path;
#endif

        auto transport = std::make_unique<StdioTransport>(
            server_path,
            std::vector<std::string>{},
            std::map<std::string, std::string>{});
        ExternalMCPClient client("mock", std::move(transport));
        REQUIRE(client.connect());

        WHEN("a tool call is executed through the client") {
            auto result = client.execute(
                "mock.echo",
                R"({"text":"hello from model test"})");

            THEN("the result contains the echoed text") {
                CHECK(result.find("hello from model test")
                      != std::string::npos);
                client.disconnect();
                end_test_log();
            }
        }
    }
}
