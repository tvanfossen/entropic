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
    GIVEN("a configured engine with filesystem tools available") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test2_tool_call_parsing");
        auto params = test_gen_params();
        params.max_tokens = 512;
        auto messages = make_messages(
            "You are a helpful assistant with filesystem tools.\n\n"
            "# Tools\n\n"
            "Here are the available tools:\n"
            "<tools>\n"
            "[{\"type\":\"function\",\"function\":{\"name\":"
            "\"filesystem.read_file\",\"description\":"
            "\"Read a file\",\"parameters\":{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\","
            "\"description\":\"File path\"}},\"required\":"
            "[\"path\"]}}}]\n"
            "</tools>\n\n"
            "For each function call, return within "
            "<tool_call></tool_call> XML tags:\n"
            "<tool_call>\n"
            "<function=example_function>\n"
            "<parameter=param_name>value</parameter>\n"
            "</function>\n"
            "</tool_call>",
            "Read the file test.txt");

        WHEN("I send a request that should trigger a tool call") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);

            auto* adapter = g_ctx.orchestrator->get_adapter(
                g_ctx.default_tier);
            REQUIRE(adapter != nullptr);
            auto raw = result.raw_content.empty()
                ? result.content : result.raw_content;

            auto parsed = adapter->parse_tool_calls(raw);

            THEN("the model emits a tool call") {
                REQUIRE(parsed.tool_calls.size() >= 1);
                auto& tc = parsed.tool_calls[0];
                CHECK(tc.name == "filesystem.read_file");
                CHECK(tc.arguments.count("path") > 0);
                end_test_log();
            }
        }
    }
}
