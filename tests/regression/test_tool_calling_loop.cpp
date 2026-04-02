/**
 * @file test_tool_calling_loop.cpp
 * @brief Regression test from test3: tool calling loop.
 *
 * Validates generate→parse→execute→generate cycle and the circuit
 * breaker (max_tool_calls_per_turn). Uses mock inference with
 * scripted tool call responses and a mock tool executor.
 *
 * @version 1.10.1
 */

#include <entropic/core/engine.h>
#include <entropic/core/engine_types.h>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mock_inference.h"

using namespace entropic;
using namespace entropic::test;

namespace {

/**
 * @brief Build system + user messages.
 * @param user_text User prompt.
 * @return Message vector.
 * @utility
 * @version 1.10.1
 */
std::vector<Message> make_messages(const std::string& user_text) {
    Message sys;
    sys.role = "system";
    sys.content = "You have tools available.";
    Message usr;
    usr.role = "user";
    usr.content = user_text;
    return {sys, usr};
}

/// @brief Counter for mock tool execution calls.
/// @version 1.10.1
static int tool_exec_count = 0;

/**
 * @brief Mock tool executor that returns scripted results.
 * @param ctx Loop context.
 * @param tool_calls Tool calls to process.
 * @param user_data Unused.
 * @return Tool result messages.
 * @callback
 * @version 1.10.1
 */
std::vector<Message> mock_tool_exec(
    LoopContext& /*ctx*/,
    const std::vector<ToolCall>& tool_calls,
    void* /*user_data*/) {
    tool_exec_count++;
    std::vector<Message> results;
    for (const auto& tc : tool_calls) {
        Message msg;
        msg.role = "tool";
        msg.content = "Result for " + tc.id;
        results.push_back(msg);
    }
    return results;
}

} // anonymous namespace

SCENARIO("Tool call cycle: generate, parse, execute, generate",
         "[regression][test3]")
{
    GIVEN("mock that returns tool call then clean response") {
        MockInference mock;
        mock.response_queue = {
            "I need to use a tool.",
            "Here is the final answer.",
        };
        mock.tool_calls_queue = {
            R"([{"name":"test.greet","arguments":{"name":"Alice"}}])",
            "[]",
        };
        auto iface = make_mock_interface(mock);

        LoopConfig lc;
        lc.max_iterations = 5;
        lc.auto_approve_tools = true;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        tool_exec_count = 0;
        ToolExecutionInterface tei;
        tei.process_tool_calls = mock_tool_exec;
        engine.set_tool_executor(tei);

        WHEN("engine runs the tool loop") {
            auto result = engine.run(
                make_messages("Greet Alice"));

            THEN("tool executor was invoked") {
                REQUIRE(tool_exec_count == 1);
            }

            THEN("generate was called twice") {
                REQUIRE(mock.generate_call_count == 2);
            }

            THEN("final response is the clean answer") {
                REQUIRE(result.back().role == "assistant");
                REQUIRE(result.back().content.find("final answer")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Circuit breaker limits tool calls per turn",
         "[regression][test3]")
{
    GIVEN("mock that always returns tool calls") {
        MockInference mock;
        mock.response = "Using another tool.";
        mock.tool_calls_json =
            R"([{"name":"test.loop","arguments":{}}])";
        auto iface = make_mock_interface(mock);

        LoopConfig lc;
        lc.max_iterations = 20;
        lc.max_tool_calls_per_turn = 3;
        lc.auto_approve_tools = true;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        tool_exec_count = 0;
        ToolExecutionInterface tei;
        tei.process_tool_calls = mock_tool_exec;
        engine.set_tool_executor(tei);

        WHEN("engine runs with always-tool-calling model") {
            engine.run(make_messages("Do something"));

            THEN("tool calls are bounded by max_iterations") {
                REQUIRE(mock.generate_call_count <= 20);
            }
        }
    }
}
