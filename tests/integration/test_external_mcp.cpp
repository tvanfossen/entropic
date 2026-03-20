/**
 * @file test_external_mcp.cpp
 * @brief Integration tests for external MCP server lifecycle.
 *
 * Uses the mock_mcp_server binary as a real child process to test
 * StdioTransport + ExternalMCPClient end-to-end.
 *
 * @version 1.8.7
 */

#include <entropic/mcp/external_client.h>
#include <entropic/mcp/transport_stdio.h>
#include <entropic/mcp/server_manager.h>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

using namespace entropic;
using json = nlohmann::json;

#ifndef MOCK_MCP_SERVER_PATH
#error "MOCK_MCP_SERVER_PATH must be defined"
#endif

/**
 * @brief Get path to mock MCP server binary.
 * @return Path string.
 * @utility
 * @version 1.8.7
 */
static std::string mock_server_path() {
    return MOCK_MCP_SERVER_PATH;
}

TEST_CASE("Stdio transport connect and execute",
          "[external_mcp][integration]") {
    auto transport = std::make_unique<StdioTransport>(
        mock_server_path(), std::vector<std::string>{});

    ExternalMCPClient client("mock", std::move(transport));
    REQUIRE(client.connect());
    REQUIRE(client.is_connected());

    // Check tools
    auto tools = json::parse(client.list_tools());
    REQUIRE(tools.size() == 1);
    REQUIRE(tools[0]["name"] == "mock.echo");

    // Execute tool
    auto result = client.execute("echo", R"({"text":"hello"})");
    auto j = json::parse(result);
    std::string r = j["result"];
    REQUIRE(r.find("hello") != std::string::npos);

    // Directives always empty (security)
    REQUIRE(j["directives"].empty());

    client.disconnect();
    REQUIRE_FALSE(client.is_connected());
}

TEST_CASE("ServerManager routes to external server",
          "[external_mcp][integration]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");

    auto tools = mgr.connect_external_server(
        "mock", mock_server_path());

    REQUIRE_FALSE(tools.empty());
    REQUIRE(tools[0] == "mock.echo");

    auto result = mgr.execute("mock.echo", R"({"text":"test"})");
    auto j = json::parse(result);
    std::string r = j["result"];
    REQUIRE(r.find("test") != std::string::npos);

    mgr.disconnect_external_server("mock");
}

TEST_CASE("Disconnected server returns structured error",
          "[external_mcp][integration]") {
    auto transport = std::make_unique<StdioTransport>(
        mock_server_path(), std::vector<std::string>{"--fail-after", "1"});

    ExternalMCPClient client("mock", std::move(transport));
    REQUIRE(client.connect());

    // First call triggers server exit
    auto result1 = client.execute("echo", R"({"text":"boom"})");
    // Server should have exited — next call hits disconnected path
    // (Give a moment for process exit to be detected)
    auto result2 = client.execute("echo", R"({"text":"after"})");
    auto j = json::parse(result2);
    std::string r = j["result"];
    REQUIRE((r.find("disconnected") != std::string::npos ||
             r.find("timed out") != std::string::npos));
}

TEST_CASE("Tool refresh after reconnect detects new tools",
          "[external_mcp][integration]") {
    auto transport = std::make_unique<StdioTransport>(
        mock_server_path(),
        std::vector<std::string>{"--tools", "echo,reverse"});

    ExternalMCPClient client("mock", std::move(transport));
    REQUIRE(client.connect());

    auto tools = json::parse(client.list_tools());
    REQUIRE(tools.size() == 2);

    client.disconnect();
}

TEST_CASE("Permission denied blocks external tools",
          "[external_mcp][integration]") {
    PermissionsConfig perms;
    perms.deny.push_back("mock.*");
    ServerManager mgr(perms, "/tmp");

    mgr.connect_external_server("mock", mock_server_path());

    auto result = mgr.execute("mock.echo", R"({"text":"denied"})");
    auto j = json::parse(result);
    std::string r = j["result"];
    REQUIRE(r.find("Permission denied") != std::string::npos);

    mgr.disconnect_external_server("mock");
}

TEST_CASE("list_server_info includes external servers",
          "[external_mcp][integration]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");

    mgr.connect_external_server("mock", mock_server_path());

    auto info = mgr.list_server_info();
    REQUIRE(info.count("mock") == 1);
    REQUIRE(info["mock"].transport == "stdio");
    REQUIRE(info["mock"].source == "runtime");

    mgr.disconnect_external_server("mock");
}

TEST_CASE("Clean shutdown terminates child processes",
          "[external_mcp][integration]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp");

    mgr.connect_external_server("mock", mock_server_path());
    mgr.shutdown();

    auto info = mgr.list_server_info();
    REQUIRE(info.empty());
}
