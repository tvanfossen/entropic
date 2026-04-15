// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_mcp_json_discovery.cpp
 * @brief MCPJsonDiscovery unit tests.
 * @version 1.8.7
 */

#include <entropic/mcp/mcp_json_discovery.h>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <set>

using namespace entropic;

/**
 * @brief Helper to write a .mcp.json file.
 * @param path File path.
 * @param content JSON content.
 * @utility
 * @version 1.8.7
 */
static void write_mcp_json(const std::filesystem::path& path,
                           const nlohmann::json& content) {
    std::ofstream f(path);
    f << content.dump();
}

TEST_CASE("Discover stdio entry from .mcp.json",
          "[mcp_json_discovery]") {
    auto tmp = std::filesystem::temp_directory_path() /
               "entropic_test_discovery";
    std::filesystem::create_directories(tmp);

    nlohmann::json mcp;
    mcp["mcpServers"]["test-server"]["command"] = "echo";
    mcp["mcpServers"]["test-server"]["args"] = {"hello"};
    write_mcp_json(tmp / ".mcp.json", mcp);

    MCPJsonDiscovery disc(tmp);
    auto result = disc.discover({});

    REQUIRE(result.size() == 1);
    REQUIRE(result[0].name == "test-server");
    REQUIRE(result[0].transport == "stdio");
    REQUIRE(result[0].command == "echo");

    std::filesystem::remove_all(tmp);
}

TEST_CASE("Discover SSE entry from .mcp.json",
          "[mcp_json_discovery]") {
    auto tmp = std::filesystem::temp_directory_path() /
               "entropic_test_discovery_sse";
    std::filesystem::create_directories(tmp);

    nlohmann::json mcp;
    mcp["mcpServers"]["remote"]["url"] = "http://127.0.0.1:8080/sse";
    write_mcp_json(tmp / ".mcp.json", mcp);

    MCPJsonDiscovery disc(tmp);
    auto result = disc.discover({});

    REQUIRE(result.size() == 1);
    REQUIRE(result[0].name == "remote");
    REQUIRE(result[0].transport == "sse");
    REQUIRE(result[0].url == "http://127.0.0.1:8080/sse");

    std::filesystem::remove_all(tmp);
}

TEST_CASE("Shadow warning skips existing server",
          "[mcp_json_discovery]") {
    auto tmp = std::filesystem::temp_directory_path() /
               "entropic_test_shadow";
    std::filesystem::create_directories(tmp);

    nlohmann::json mcp;
    mcp["mcpServers"]["filesystem"]["command"] = "fake";
    write_mcp_json(tmp / ".mcp.json", mcp);

    MCPJsonDiscovery disc(tmp);
    std::set<std::string> existing = {"filesystem"};
    auto result = disc.discover(existing);

    REQUIRE(result.empty());

    std::filesystem::remove_all(tmp);
}

TEST_CASE("Missing command skips stdio entry",
          "[mcp_json_discovery]") {
    auto tmp = std::filesystem::temp_directory_path() /
               "entropic_test_no_cmd";
    std::filesystem::create_directories(tmp);

    nlohmann::json mcp;
    mcp["mcpServers"]["bad"]["type"] = "stdio";
    // No command field
    write_mcp_json(tmp / ".mcp.json", mcp);

    MCPJsonDiscovery disc(tmp);
    auto result = disc.discover({});

    REQUIRE(result.empty());

    std::filesystem::remove_all(tmp);
}

TEST_CASE("Env blocklist filters dangerous variables",
          "[mcp_json_discovery]") {
    REQUIRE(is_blocked_env_var("LD_PRELOAD"));
    REQUIRE(is_blocked_env_var("LD_LIBRARY_PATH"));
    REQUIRE(is_blocked_env_var("PATH"));
    REQUIRE(is_blocked_env_var("HOME"));
    REQUIRE(is_blocked_env_var("ENTROPIC_CONFIG"));
    REQUIRE(is_blocked_env_var("DYLD_INSERT_LIBRARIES"));

    REQUIRE_FALSE(is_blocked_env_var("API_KEY"));
    REQUIRE_FALSE(is_blocked_env_var("NODE_ENV"));
    REQUIRE_FALSE(is_blocked_env_var("MY_VAR"));
}

TEST_CASE("compute_socket_path is deterministic",
          "[mcp_json_discovery]") {
    auto p1 = compute_socket_path("/home/user/project");
    auto p2 = compute_socket_path("/home/user/project");
    REQUIRE(p1 == p2);
}

TEST_CASE("compute_socket_path differs for different dirs",
          "[mcp_json_discovery]") {
    auto p1 = compute_socket_path("/home/user/project-a");
    auto p2 = compute_socket_path("/home/user/project-b");
    REQUIRE(p1 != p2);
}

TEST_CASE("Env blocklist applied to .mcp.json entries",
          "[mcp_json_discovery]") {
    auto tmp = std::filesystem::temp_directory_path() /
               "entropic_test_env_block";
    std::filesystem::create_directories(tmp);

    nlohmann::json mcp;
    mcp["mcpServers"]["srv"]["command"] = "echo";
    mcp["mcpServers"]["srv"]["env"]["API_KEY"] = "secret";
    mcp["mcpServers"]["srv"]["env"]["LD_PRELOAD"] = "/evil.so";
    mcp["mcpServers"]["srv"]["env"]["ENTROPIC_DEBUG"] = "1";
    write_mcp_json(tmp / ".mcp.json", mcp);

    MCPJsonDiscovery disc(tmp);
    auto result = disc.discover({});

    REQUIRE(result.size() == 1);
    REQUIRE(result[0].env.count("API_KEY") == 1);
    REQUIRE(result[0].env.count("LD_PRELOAD") == 0);
    REQUIRE(result[0].env.count("ENTROPIC_DEBUG") == 0);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("Malformed JSON skips gracefully",
          "[mcp_json_discovery]") {
    auto tmp = std::filesystem::temp_directory_path() /
               "entropic_test_malformed";
    std::filesystem::create_directories(tmp);

    std::ofstream f(tmp / ".mcp.json");
    f << "not valid json!!!";
    f.close();

    MCPJsonDiscovery disc(tmp);
    auto result = disc.discover({});

    REQUIRE(result.empty());

    std::filesystem::remove_all(tmp);
}

TEST_CASE("Explicit type field overrides inference",
          "[mcp_json_discovery]") {
    auto tmp = std::filesystem::temp_directory_path() /
               "entropic_test_type_override";
    std::filesystem::create_directories(tmp);

    nlohmann::json mcp;
    // Has both url and command, but type=sse overrides
    mcp["mcpServers"]["srv"]["type"] = "sse";
    mcp["mcpServers"]["srv"]["url"] = "http://localhost:8080/sse";
    mcp["mcpServers"]["srv"]["command"] = "ignored";
    write_mcp_json(tmp / ".mcp.json", mcp);

    MCPJsonDiscovery disc(tmp);
    auto result = disc.discover({});

    REQUIRE(result.size() == 1);
    REQUIRE(result[0].transport == "sse");

    std::filesystem::remove_all(tmp);
}
