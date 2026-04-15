// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_server_base.cpp
 * @brief MCPServerBase unit tests.
 * @version 1.8.5
 */

#include <entropic/mcp/server_base.h>
#include <entropic/mcp/tool_base.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace entropic;

// ── Test tools ───────────────────────────────────────────

/**
 * @brief Simple tool that returns fixed text.
 * @version 1.8.5
 */
class SimpleTool : public ToolBase {
public:
    /**
     * @brief Construct.
     * @version 1.8.5
     */
    SimpleTool()
        : ToolBase(ToolDefinition{
              "simple",
              "A simple test tool",
              R"({"type":"object","properties":{}})"
          }) {}

    /**
     * @brief Execute: return fixed text.
     * @param args_json Arguments (unused).
     * @return ServerResponse.
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& /*args_json*/) override {
        return ServerResponse{"simple result", {}};
    }
};

/**
 * @brief Tool that declares an anchor key.
 * @version 1.8.5
 */
class AnchoredTool : public ToolBase {
public:
    /**
     * @brief Construct.
     * @version 1.8.5
     */
    AnchoredTool()
        : ToolBase(ToolDefinition{
              "read_file",
              "Reads a file",
              R"({"type":"object","properties":{"path":{"type":"string"}}})"
          }) {}

    /**
     * @brief Execute: return file content.
     * @param args_json Arguments.
     * @return ServerResponse.
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& /*args_json*/) override {
        return ServerResponse{"file content here", {}};
    }

    /**
     * @brief Anchor key: "file:{path}".
     * @param args_json Arguments with path.
     * @return Anchor key string.
     * @version 1.8.5
     */
    std::string anchor_key(const std::string& args_json) const override {
        auto j = nlohmann::json::parse(args_json);
        return "file:" + j.value("path", "unknown");
    }
};

// ── Test server ──────────────────────────────────────────

/**
 * @brief Concrete test server with both tools.
 * @version 1.8.5
 */
class TestServer : public MCPServerBase {
public:
    /**
     * @brief Construct and register tools.
     * @version 1.8.5
     */
    TestServer()
        : MCPServerBase("test") {
        register_tool(&simple_);
        register_tool(&anchored_);
    }

private:
    SimpleTool simple_;      ///< Simple tool
    AnchoredTool anchored_;  ///< Anchored tool
};

// ── Tests ────────────────────────────────────────────────

TEST_CASE("Server registers tools", "[server_base]") {
    TestServer server;
    auto tools_json = server.list_tools();
    auto tools = nlohmann::json::parse(tools_json);
    REQUIRE(tools.size() == 2);
}

TEST_CASE("Execute dispatches to tool", "[server_base]") {
    TestServer server;
    auto result = server.execute("simple", "{}");
    auto j = nlohmann::json::parse(result);
    REQUIRE(j["result"] == "simple result");
}

TEST_CASE("Unknown tool returns error", "[server_base]") {
    TestServer server;
    auto result = server.execute("nonexistent", "{}");
    auto j = nlohmann::json::parse(result);
    std::string r = j["result"];
    REQUIRE(r.find("Error") != std::string::npos);
}

TEST_CASE("ServerResponse JSON envelope", "[server_base]") {
    TestServer server;
    auto result = server.execute("simple", "{}");
    auto j = nlohmann::json::parse(result);
    REQUIRE(j.contains("result"));
    REQUIRE(j.contains("directives"));
    REQUIRE(j["directives"].is_array());
}

TEST_CASE("Context anchor auto-injection", "[server_base]") {
    TestServer server;
    auto result = server.execute("read_file", R"({"path":"src/main.cpp"})");
    auto j = nlohmann::json::parse(result);
    REQUIRE(j["directives"].size() >= 1);
}

TEST_CASE("No anchor by default", "[server_base]") {
    TestServer server;
    auto result = server.execute("simple", "{}");
    auto j = nlohmann::json::parse(result);
    REQUIRE(j["directives"].empty());
}

TEST_CASE("Default permission pattern", "[server_base]") {
    TestServer server;
    auto pattern = server.get_permission_pattern(
        "test.simple", "{}");
    REQUIRE(pattern == "test.simple");
}

TEST_CASE("Default skip duplicate check", "[server_base]") {
    TestServer server;
    REQUIRE_FALSE(server.skip_duplicate_check("simple"));
}
