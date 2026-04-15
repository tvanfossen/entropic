// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_tool_executor_authorization.cpp
 * @brief ToolExecutor MCP authorization integration tests.
 * @version 1.9.4
 */

#include <entropic/mcp/mcp_authorization.h>
#include <entropic/mcp/server_base.h>
#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/tool_executor.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace entropic;

// ── Test tools ───────────────────────────────────────────────

/**
 * @brief Read-only test tool.
 * @version 1.9.4
 */
class ReadOnlyTool : public ToolBase {
public:
    /**
     * @brief Construct.
     * @version 1.9.4
     */
    ReadOnlyTool() : ToolBase(ToolDefinition{
        "read_thing", "Reads a thing",
        R"({"type":"object","properties":{}})"
    }) {}

    /**
     * @brief Read access only.
     * @return MCPAccessLevel::READ.
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Execute: return "read_ok".
     * @param args_json Arguments (unused).
     * @return ServerResponse.
     * @version 1.9.4
     */
    ServerResponse execute(const std::string&) override {
        return ServerResponse{"read_ok", {}};
    }
};

/**
 * @brief Write test tool (default access level).
 * @version 1.9.4
 */
class WriteTool : public ToolBase {
public:
    /**
     * @brief Construct.
     * @version 1.9.4
     */
    WriteTool() : ToolBase(ToolDefinition{
        "write_thing", "Writes a thing",
        R"({"type":"object","properties":{}})"
    }) {}

    /**
     * @brief Execute: return "write_ok".
     * @param args_json Arguments (unused).
     * @return ServerResponse.
     * @version 1.9.4
     */
    ServerResponse execute(const std::string&) override {
        return ServerResponse{"write_ok", {}};
    }
};

/**
 * @brief Test server with read and write tools.
 * @version 1.9.4
 */
class AuthTestServer : public MCPServerBase {
public:
    /**
     * @brief Construct and register tools.
     * @version 1.9.4
     */
    AuthTestServer() : MCPServerBase("authtest") {
        register_tool(&read_tool_);
        register_tool(&write_tool_);
    }
private:
    ReadOnlyTool read_tool_;  ///< Read-only tool
    WriteTool write_tool_;    ///< Write tool
};

// ── Helper ───────────────────────────────────────────────────

/**
 * @brief Build a ToolCall for the given tool name.
 * @param name Tool name.
 * @return ToolCall struct.
 * @internal
 * @version 1.9.4
 */
static ToolCall make_call(const std::string& name) {
    ToolCall tc;
    tc.id = "test-1";
    tc.name = name;
    return tc;
}

// ── Tests ────────────────────────────────────────────────────

TEST_CASE("Authorization check: denied tool returns error",
          "[mcp][tool_executor][authorization]") {
    PermissionsConfig perms;
    perms.auto_approve = true;
    ServerManager sm(perms, ".");
    sm.register_server(std::make_unique<AuthTestServer>());
    sm.initialize();

    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb{};
    ToolExecutor exec(sm, lc, cb);

    MCPAuthorizationManager auth;
    auth.register_identity("eng");
    // eng has no keys — everything denied
    exec.set_authorization_manager(&auth);

    LoopContext ctx;
    ctx.locked_tier = "eng";
    auto results = exec.process_tool_calls(
        ctx, {make_call("authtest.read_thing")});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("denied") !=
            std::string::npos);
}

TEST_CASE("Authorization check: authorized tool executes",
          "[mcp][tool_executor][authorization]") {
    PermissionsConfig perms;
    perms.auto_approve = true;
    ServerManager sm(perms, ".");
    sm.register_server(std::make_unique<AuthTestServer>());
    sm.initialize();

    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb{};
    ToolExecutor exec(sm, lc, cb);

    MCPAuthorizationManager auth;
    auth.register_identity("eng");
    auth.grant("eng", "authtest.*", MCPAccessLevel::WRITE);
    exec.set_authorization_manager(&auth);

    LoopContext ctx;
    ctx.locked_tier = "eng";
    auto results = exec.process_tool_calls(
        ctx, {make_call("authtest.write_thing")});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("write_ok") !=
            std::string::npos);
}

TEST_CASE("No enforcement without key set",
          "[mcp][tool_executor][authorization]") {
    PermissionsConfig perms;
    perms.auto_approve = true;
    ServerManager sm(perms, ".");
    sm.register_server(std::make_unique<AuthTestServer>());
    sm.initialize();

    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb{};
    ToolExecutor exec(sm, lc, cb);

    MCPAuthorizationManager auth;
    // "eng" is NOT registered — no enforcement
    exec.set_authorization_manager(&auth);

    LoopContext ctx;
    ctx.locked_tier = "eng";
    auto results = exec.process_tool_calls(
        ctx, {make_call("authtest.write_thing")});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("write_ok") !=
            std::string::npos);
}

TEST_CASE("Denial message contains tool name and level",
          "[mcp][tool_executor][authorization]") {
    PermissionsConfig perms;
    perms.auto_approve = true;
    ServerManager sm(perms, ".");
    sm.register_server(std::make_unique<AuthTestServer>());
    sm.initialize();

    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb{};
    ToolExecutor exec(sm, lc, cb);

    MCPAuthorizationManager auth;
    auth.register_identity("eng");
    exec.set_authorization_manager(&auth);

    LoopContext ctx;
    ctx.locked_tier = "eng";
    auto results = exec.process_tool_calls(
        ctx, {make_call("authtest.write_thing")});
    REQUIRE(results.size() == 1);
    auto& msg = results[0].content;
    REQUIRE(msg.find("authtest.write_thing") !=
            std::string::npos);
    REQUIRE(msg.find("WRITE") != std::string::npos);
    REQUIRE(msg.find("delegate") != std::string::npos);
}

TEST_CASE("READ tool with READ access succeeds",
          "[mcp][tool_executor][authorization]") {
    PermissionsConfig perms;
    perms.auto_approve = true;
    ServerManager sm(perms, ".");
    sm.register_server(std::make_unique<AuthTestServer>());
    sm.initialize();

    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb{};
    ToolExecutor exec(sm, lc, cb);

    MCPAuthorizationManager auth;
    auth.register_identity("eng");
    auth.grant("eng", "authtest.*", MCPAccessLevel::READ);
    exec.set_authorization_manager(&auth);

    LoopContext ctx;
    ctx.locked_tier = "eng";
    auto results = exec.process_tool_calls(
        ctx, {make_call("authtest.read_thing")});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("read_ok") !=
            std::string::npos);
}

TEST_CASE("WRITE tool denied with READ-only access",
          "[mcp][tool_executor][authorization]") {
    PermissionsConfig perms;
    perms.auto_approve = true;
    ServerManager sm(perms, ".");
    sm.register_server(std::make_unique<AuthTestServer>());
    sm.initialize();

    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb{};
    ToolExecutor exec(sm, lc, cb);

    MCPAuthorizationManager auth;
    auth.register_identity("eng");
    auth.grant("eng", "authtest.*", MCPAccessLevel::READ);
    exec.set_authorization_manager(&auth);

    LoopContext ctx;
    ctx.locked_tier = "eng";
    auto results = exec.process_tool_calls(
        ctx, {make_call("authtest.write_thing")});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("denied") !=
            std::string::npos);
}
