/**
 * @file test_tool_registry.cpp
 * @brief ToolRegistry unit tests.
 * @version 1.8.5
 */

#include <entropic/mcp/server_base.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/tool_registry.h>
#include <catch2/catch_test_macros.hpp>

using namespace entropic;

// ── Test tool ────────────────────────────────────────────

/**
 * @brief Concrete test tool that returns a fixed result.
 * @version 1.8.5
 */
class EchoTool : public ToolBase {
public:
    /**
     * @brief Construct an echo tool.
     * @param name Tool name.
     * @version 1.8.5
     */
    explicit EchoTool(const std::string& name)
        : ToolBase(ToolDefinition{
              name,
              "Echo tool for testing",
              R"({"type":"object","properties":{"text":{"type":"string"}}})"
          }) {}

    /**
     * @brief Execute: return the text argument.
     * @param args_json JSON arguments.
     * @return ServerResponse with echoed text.
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override {
        ServerResponse resp;
        resp.result = "echo: " + args_json;
        return resp;
    }
};

// ── Tests ────────────────────────────────────────────────

TEST_CASE("Register and dispatch tool", "[tool_registry]") {
    ToolRegistry registry;
    EchoTool tool("greet");
    registry.register_tool(&tool);

    REQUIRE(registry.has_tool("greet"));
    auto resp = registry.dispatch("greet", R"({"text":"hi"})");
    REQUIRE(resp.result.find("echo:") != std::string::npos);
}

TEST_CASE("has_tool returns false for unknown", "[tool_registry]") {
    ToolRegistry registry;
    REQUIRE_FALSE(registry.has_tool("nonexistent"));
}

TEST_CASE("get_tools_json returns definitions", "[tool_registry]") {
    ToolRegistry registry;
    EchoTool t1("alpha");
    EchoTool t2("beta");
    registry.register_tool(&t1);
    registry.register_tool(&t2);

    auto json = registry.get_tools_json();
    REQUIRE(json.find("alpha") != std::string::npos);
    REQUIRE(json.find("beta") != std::string::npos);
}

TEST_CASE("Dispatch unknown returns error", "[tool_registry]") {
    ToolRegistry registry;
    auto resp = registry.dispatch("missing", "{}");
    REQUIRE(resp.result.find("Error") != std::string::npos);
    REQUIRE(resp.result.find("missing") != std::string::npos);
}

TEST_CASE("Duplicate registration warns", "[tool_registry]") {
    ToolRegistry registry;
    EchoTool t1("dupe");
    EchoTool t2("dupe");
    registry.register_tool(&t1);
    registry.register_tool(&t2); // should warn, not crash
    REQUIRE(registry.has_tool("dupe"));
}
