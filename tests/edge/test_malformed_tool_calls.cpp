// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_malformed_tool_calls.cpp
 * @brief Edge case tests for malformed tool call handling.
 *
 * Validates that the tool system returns structured errors (never crashes)
 * when given malformed inputs: bad JSON, unknown tools, empty args, wrong
 * types, empty results, binary data.
 *
 * @version 1.9.14
 */

#include <entropic/mcp/server_base.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/tool_registry.h>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace entropic;

// ── Test tools ──────────────────────────────────────────

/**
 * @brief Tool that echoes args for dispatch testing.
 * @internal
 * @version 1.9.14
 */
class EdgeEchoTool : public ToolBase {
public:
    /**
     * @brief Construct with name.
     * @param name Tool name.
     * @version 1.9.14
     */
    explicit EdgeEchoTool(const std::string& name)
        : ToolBase(ToolDefinition{
              name,
              "Echo tool for edge case testing",
              R"({"type":"object","properties":{"text":{"type":"string"}}})"
          }) {}

    /**
     * @brief Execute: return args verbatim.
     * @param args_json JSON arguments.
     * @return ServerResponse with echoed args.
     * @version 1.9.14
     */
    ServerResponse execute(const std::string& args_json) override {
        ServerResponse resp;
        resp.result = args_json;
        return resp;
    }
};

/**
 * @brief Tool that returns an empty result string.
 * @internal
 * @version 1.9.14
 */
class EmptyResultTool : public ToolBase {
public:
    /**
     * @brief Construct with name.
     * @version 1.9.14
     */
    EmptyResultTool()
        : ToolBase(ToolDefinition{
              "empty_result",
              "Returns empty string",
              R"({"type":"object","properties":{}})"
          }) {}

    /**
     * @brief Execute: return empty result.
     * @param args_json JSON arguments (unused).
     * @return ServerResponse with empty result.
     * @version 1.9.14
     */
    ServerResponse execute(const std::string& /*args_json*/) override {
        return ServerResponse{"", {}};
    }
};

/**
 * @brief Tool that returns result containing null bytes.
 * @internal
 * @version 1.9.14
 */
class BinaryResultTool : public ToolBase {
public:
    /**
     * @brief Construct with name.
     * @version 1.9.14
     */
    BinaryResultTool()
        : ToolBase(ToolDefinition{
              "binary_result",
              "Returns binary data with null bytes",
              R"({"type":"object","properties":{}})"
          }) {}

    /**
     * @brief Execute: return result with embedded nulls.
     * @param args_json JSON arguments (unused).
     * @return ServerResponse with binary content.
     * @version 1.9.14
     */
    ServerResponse execute(const std::string& /*args_json*/) override {
        std::string binary_data = "before";
        binary_data.push_back('\0');
        binary_data += "after";
        return ServerResponse{binary_data, {}};
    }
};

// ── Tests ───────────────────────────────────────────────

SCENARIO("Unknown tool dispatch returns error", "[edge][tool]") {
    GIVEN("An empty tool registry") {
        ToolRegistry registry;

        WHEN("dispatching a nonexistent tool") {
            auto resp = registry.dispatch("nonexistent.tool", "{}");

            THEN("result contains error, no crash") {
                REQUIRE_FALSE(resp.result.empty());
                REQUIRE(resp.result.find("nonexistent.tool") !=
                        std::string::npos);
            }
        }
    }
}

SCENARIO("Empty tool name dispatch returns error", "[edge][tool]") {
    GIVEN("A registry with one tool") {
        ToolRegistry registry;
        EdgeEchoTool tool("test_tool");
        registry.register_tool(&tool);

        WHEN("dispatching with empty name") {
            auto resp = registry.dispatch("", "{}");

            THEN("result contains error, no crash") {
                REQUIRE_FALSE(resp.result.empty());
            }
        }
    }
}

SCENARIO("Malformed JSON args reach tool", "[edge][tool]") {
    GIVEN("A registry with an echo tool") {
        ToolRegistry registry;
        EdgeEchoTool tool("echo");
        registry.register_tool(&tool);

        WHEN("dispatching with non-JSON args") {
            auto resp = registry.dispatch("echo", "{not json}");

            THEN("dispatch completes without crash") {
                // Tool receives raw args — schema validation
                // happens upstream. Registry does not crash.
                REQUIRE_FALSE(resp.result.empty());
            }
        }
    }
}

SCENARIO("Dispatch with empty args string", "[edge][tool]") {
    GIVEN("A registry with an echo tool") {
        ToolRegistry registry;
        EdgeEchoTool tool("echo");
        registry.register_tool(&tool);

        WHEN("dispatching with empty string args") {
            auto resp = registry.dispatch("echo", "");

            THEN("tool executes, no crash") {
                // Echo returns args verbatim — empty string
                REQUIRE(resp.result.empty());
            }
        }
    }
}

SCENARIO("Tool returning empty result", "[edge][tool]") {
    GIVEN("A registry with an empty-result tool") {
        ToolRegistry registry;
        EmptyResultTool tool;
        registry.register_tool(&tool);

        WHEN("dispatching the empty-result tool") {
            auto resp = registry.dispatch("empty_result", "{}");

            THEN("result is empty string, no crash or null deref") {
                REQUIRE(resp.result.empty());
                REQUIRE(resp.directives.empty());
            }
        }
    }
}

SCENARIO("Tool returning binary data with nulls", "[edge][tool]") {
    GIVEN("A registry with a binary-result tool") {
        ToolRegistry registry;
        BinaryResultTool tool;
        registry.register_tool(&tool);

        WHEN("dispatching the binary-result tool") {
            auto resp = registry.dispatch("binary_result", "{}");

            THEN("result preserves data, no string corruption") {
                // std::string preserves embedded nulls
                REQUIRE(resp.result.size() > 6);
                REQUIRE(resp.result.find("before") != std::string::npos);
            }
        }
    }
}

SCENARIO("Dispatch with extremely long tool name", "[edge][tool]") {
    GIVEN("An empty registry") {
        ToolRegistry registry;

        WHEN("dispatching a 10KB tool name") {
            std::string long_name(10240, 'x');
            auto resp = registry.dispatch(long_name, "{}");

            THEN("returns error without crash or OOM") {
                REQUIRE_FALSE(resp.result.empty());
            }
        }
    }
}

SCENARIO("Dispatch with extremely large args", "[edge][tool]") {
    GIVEN("A registry with an echo tool") {
        ToolRegistry registry;
        EdgeEchoTool tool("echo");
        registry.register_tool(&tool);

        WHEN("dispatching with 1MB args string") {
            std::string large_args(1024 * 1024, 'A');
            auto resp = registry.dispatch("echo", large_args);

            THEN("tool processes without crash") {
                REQUIRE(resp.result.size() == large_args.size());
            }
        }
    }
}
