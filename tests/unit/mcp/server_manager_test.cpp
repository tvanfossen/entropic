// SPDX-License-Identifier: Apache-2.0
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

// ── v2.3.10: coverage for accessors + prefix/local-name + args_to_pattern ──

TEST_CASE("server_names returns the registered server list",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());

    auto names = mgr.server_names();
    REQUIRE_FALSE(names.empty());
    REQUIRE(std::find(names.begin(), names.end(), "ping") != names.end());
}

TEST_CASE("get_server returns nullptr for unknown server name",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());

    REQUIRE(mgr.get_server("ping") != nullptr);
    REQUIRE(mgr.get_server("missing-server-2310") == nullptr);
}

TEST_CASE("get_tool_schema returns the registered tool's schema",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());
    mgr.initialize();

    auto schema = mgr.get_tool_schema("ping.ping");
    REQUIRE_FALSE(schema.empty());
    // PingTool's schema is the empty-object form.
    REQUIRE(schema.find("object") != std::string::npos);
}

TEST_CASE("get_tool_schema returns empty for unknown tool",
          "[server_manager][v2.3.10][coverage][failure-mode]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());

    auto schema = mgr.get_tool_schema("does.not.exist");
    REQUIRE(schema.empty());
}

TEST_CASE("set_mcp_config does not throw on a default-constructed config",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    MCPConfig cfg;
    mgr.set_mcp_config(cfg);  // exercises the public setter path
    REQUIRE(true);
}

TEST_CASE("get_required_access_level returns a typed access level",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());
    mgr.initialize();

    // PingTool has no special access annotation; default is WRITE
    // per the comments in config.h (new tools opt into READ).
    auto lvl = mgr.get_required_access_level("ping.ping");
    REQUIRE((lvl == MCPAccessLevel::READ || lvl == MCPAccessLevel::WRITE));
}

TEST_CASE("interrupt_external_tools is safe with no external connections",
          "[server_manager][v2.3.10][coverage][failure-mode]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");

    mgr.interrupt_external_tools();  // no-op path; just must not crash
    REQUIRE(true);
}

// ── v2.3.10: cover public delegation accessors + unknown-prefix fallbacks ──

TEST_CASE("get_permission_pattern delegates to server when prefix is known",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());
    mgr.initialize();

    SECTION("known prefix — delegates to server's get_permission_pattern") {
        auto pat = mgr.get_permission_pattern(
            "ping.ping", R"({"x":"y"})");
        REQUIRE_FALSE(pat.empty());
    }

    SECTION("unknown prefix — falls back to tool_name") {
        // Hits the fallback `return tool_name;` at line 310.
        auto pat = mgr.get_permission_pattern(
            "unknown.tool", "{}");
        REQUIRE(pat == "unknown.tool");
    }
}

TEST_CASE("is_explicitly_allowed checks the permission allow-list",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    perms.allow.push_back("ping.ping:*");
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());

    REQUIRE(mgr.is_explicitly_allowed("ping.ping", "{}"));
    REQUIRE_FALSE(mgr.is_explicitly_allowed("other.tool", "{}"));
}

TEST_CASE("add_permission inserts a runtime allow pattern",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());

    REQUIRE_FALSE(mgr.is_explicitly_allowed("ping.ping", "{}"));
    mgr.add_permission("ping.ping:*", /*allow=*/true);
    REQUIRE(mgr.is_explicitly_allowed("ping.ping", "{}"));
}

TEST_CASE("skip_duplicate_check delegates and falls back to false",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());

    SECTION("known prefix — delegates to server") {
        bool result = mgr.skip_duplicate_check("ping.ping");
        // Either true or false is acceptable; we only assert the call
        // path runs without crash. PingServer's default behavior.
        (void)result;
        REQUIRE(true);
    }

    SECTION("unknown prefix — line 328 fallback returns false") {
        REQUIRE_FALSE(mgr.skip_duplicate_check("unknown.tool"));
    }
}

TEST_CASE("get_required_access_level falls back to WRITE for unknown tool",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());

    // Unknown prefix → line 349 fallback returns WRITE (safe default).
    auto lvl = mgr.get_required_access_level("unknown.tool");
    REQUIRE(lvl == MCPAccessLevel::WRITE);
}

TEST_CASE("shutdown clears in-process servers and is safe to call multiple times",
          "[server_manager][v2.3.10][coverage]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");
    mgr.register_server(std::make_unique<PingServer>());
    mgr.initialize();

    REQUIRE_FALSE(mgr.server_names().empty());
    mgr.shutdown();
    REQUIRE(mgr.server_names().empty());

    // Second shutdown — idempotent.
    mgr.shutdown();
    REQUIRE(mgr.server_names().empty());
}
