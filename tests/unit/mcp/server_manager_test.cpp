// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_server_manager.cpp
 * @brief ServerManager unit tests.
 * @version 1.8.5
 */

#include <entropic/mcp/server_base.h>
#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/tool_base.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace entropic;

// ── Test fixtures ────────────────────────────────────────

/**
 * @brief Minimal test tool.
 * @version 1.8.5
 */
class PingTool : public ToolBase {
public:
    /**
     * @brief Construct.
     * @version 1.8.5
     */
    PingTool() : ToolBase(ToolDefinition{
        "ping", "Returns pong",
        R"({"type":"object","properties":{}})"
    }) {}

    /**
     * @brief Execute: return "pong".
     * @param args_json Arguments (unused).
     * @return ServerResponse.
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& /*args_json*/) override {
        return ServerResponse{"pong", {}};
    }
};

/**
 * @brief Test server wrapping PingTool.
 * @version 1.8.5
 */
class PingServer : public MCPServerBase {
public:
    /**
     * @brief Construct and register tool.
     * @version 1.8.5
     */
    PingServer() : MCPServerBase("ping") {
        register_tool(&tool_);
    }
private:
    PingTool tool_; ///< The tool
};

// ── Tests ────────────────────────────────────────────────

TEST_CASE("Register builtin server", "[server_manager]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());
    mgr.initialize();

    auto tools = mgr.list_tools();
    auto j = nlohmann::json::parse(tools);
    REQUIRE(j.size() == 1);
    REQUIRE(j[0]["name"] == "ping.ping");
}

TEST_CASE("Tool routing", "[server_manager]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());
    mgr.initialize();

    auto result = mgr.execute("ping.ping", "{}");
    auto j = nlohmann::json::parse(result);
    REQUIRE(j["result"] == "pong");
}

TEST_CASE("Unknown server returns error", "[server_manager]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.initialize();

    auto result = mgr.execute("unknown.tool", "{}");
    auto j = nlohmann::json::parse(result);
    std::string r = j["result"];
    REQUIRE(r.find("Error") != std::string::npos);
}

TEST_CASE("Permission denied blocks execution", "[server_manager]") {
    PermissionsConfig perms;
    perms.deny.push_back("ping.*");
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());
    mgr.initialize();

    auto result = mgr.execute("ping.ping", "{}");
    auto j = nlohmann::json::parse(result);
    std::string r = j["result"];
    REQUIRE(r.find("Permission denied") != std::string::npos);
}
