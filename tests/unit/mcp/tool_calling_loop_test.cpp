// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_tool_calling_loop.cpp
 * @brief End-to-end integration test: generate → parse → execute → generate.
 * @version 1.8.5
 */

#include <entropic/mcp/server_base.h>
#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/tool_executor.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace entropic;

// ── Test fixtures ────────────────────────────────────────

/**
 * @brief Simple tool that returns fixed content.
 * @version 1.8.5
 */
class GreetTool : public ToolBase {
public:
    /**
     * @brief Construct.
     * @version 1.8.5
     */
    GreetTool() : ToolBase(ToolDefinition{
        "greet", "Returns a greeting",
        R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})"
    }) {}

    /**
     * @brief Execute: return greeting.
     * @param args_json Arguments.
     * @return ServerResponse.
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        auto j = nlohmann::json::parse(args_json);
        auto name = j.value("name", "world");
        return ServerResponse{"Hello, " + name + "!", {}};
    }
};

/**
 * @brief Test server wrapping GreetTool.
 * @version 1.8.5
 */
class GreetServer : public MCPServerBase {
public:
    /**
     * @brief Construct and register tool.
     * @version 1.8.5
     */
    GreetServer() : MCPServerBase("greet") {
        register_tool(&tool_);
    }
private:
    GreetTool tool_; ///< The greeting tool
};

/**
 * @brief Tool that returns an error.
 * @version 1.8.5
 */
class FailTool : public ToolBase {
public:
    /**
     * @brief Construct.
     * @version 1.8.5
     */
    FailTool() : ToolBase(ToolDefinition{
        "fail", "Always fails",
        R"({"type":"object","properties":{}})"
    }) {}

    /**
     * @brief Execute: return error.
     * @param args_json Arguments (unused).
     * @return ServerResponse.
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& /*args_json*/) override {
        return ServerResponse{"Error: something went wrong", {}};
    }
};

/**
 * @brief Test server wrapping FailTool.
 * @version 1.8.5
 */
class FailServer : public MCPServerBase {
public:
    /**
     * @brief Construct and register tool.
     * @version 1.8.5
     */
    FailServer() : MCPServerBase("fail") {
        register_tool(&tool_);
    }
private:
    FailTool tool_; ///< The failing tool
};

// ── Tests ────────────────────────────────────────────────

TEST_CASE("Tool executor processes calls via ServerManager",
          "[tool_calling_loop]") {
    // Set up server manager with greet server
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<GreetServer>());
    mgr.initialize();

    // Set up tool executor
    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    // Create a tool call
    ToolCall call;
    call.id = "tc-001";
    call.name = "greet.greet";
    call.arguments["name"] = "Alice";

    // Process
    LoopContext ctx;
    auto results = executor.process_tool_calls(ctx, {call});

    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("Hello, Alice!") != std::string::npos);
    REQUIRE(ctx.metrics.tool_calls == 1);
}

TEST_CASE("Multiple tool calls in batch",
          "[tool_calling_loop]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<GreetServer>());
    mgr.initialize();

    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    ToolCall c1;
    c1.id = "tc-a";
    c1.name = "greet.greet";
    c1.arguments["name"] = "Alice";

    ToolCall c2;
    c2.id = "tc-b";
    c2.name = "greet.greet";
    c2.arguments["name"] = "Bob";

    LoopContext ctx;
    auto results = executor.process_tool_calls(ctx, {c1, c2});
    REQUIRE(results.size() == 2);
    REQUIRE(results[0].content.find("Alice") != std::string::npos);
    REQUIRE(results[1].content.find("Bob") != std::string::npos);
    REQUIRE(ctx.metrics.tool_calls == 2);
}

TEST_CASE("Permission denied returns feedback",
          "[tool_calling_loop]") {
    PermissionsConfig perms;
    perms.deny.push_back("greet.*");
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<GreetServer>());
    mgr.initialize();

    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    ToolCall call;
    call.id = "tc-002";
    call.name = "greet.greet";
    call.arguments["name"] = "Bob";

    LoopContext ctx;
    auto results = executor.process_tool_calls(ctx, {call});

    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("Permission denied") != std::string::npos);
}

TEST_CASE("Error result not cached for dedup",
          "[tool_calling_loop]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<FailServer>());
    mgr.initialize();

    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    ToolCall call;
    call.id = "tc-003";
    call.name = "fail.fail";

    LoopContext ctx;
    // First call (error, not cached)
    executor.process_tool_calls(ctx, {call});
    // Second identical call (should execute again, not be "duplicate")
    auto results = executor.process_tool_calls(ctx, {call});

    REQUIRE(results.size() == 1);
    // Should re-execute, not say "already called"
    REQUIRE(results[0].content.find("already called") == std::string::npos);
}
