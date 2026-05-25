// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_diagnostics.cpp
 * @brief DiagnosticsServer unit tests.
 * @version 1.8.5
 */

#include <entropic/mcp/servers/diagnostics.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace entropic;

// ── Helpers ─────────────────────────────────────────────

/**
 * @brief Create a DiagnosticsServer with TEST_DATA_DIR.
 * @internal
 * @return Configured DiagnosticsServer.
 * @version 1.8.5
 */
static DiagnosticsServer make_diagnostics_server() {
    return DiagnosticsServer("/tmp", TEST_DATA_DIR);
}

/**
 * @brief Parse the "result" field from execute() JSON envelope.
 * @internal
 * @param envelope Raw JSON string returned by execute().
 * @return The "result" string value.
 * @version 1.8.5
 */
static std::string parse_result(const std::string& envelope) {
    auto j = json::parse(envelope);
    return j["result"].get<std::string>();
}

// ── Tests ───────────────────────────────────────────────

TEST_CASE("Diagnostics returns placeholder", "[diagnostics]") {
    auto server = make_diagnostics_server();
    json args;
    args["file_path"] = "test.cpp";

    auto result = parse_result(server.execute("diagnostics", args.dump()));
    REQUIRE_FALSE(result.empty());
}

TEST_CASE("Check errors returns placeholder", "[diagnostics]") {
    auto server = make_diagnostics_server();
    json args;
    args["file_path"] = "test.cpp";

    auto result = parse_result(server.execute("check_errors", args.dump()));
    REQUIRE_FALSE(result.empty());
}

TEST_CASE("Diagnostics server registers 2 tools", "[diagnostics]") {
    auto server = make_diagnostics_server();
    auto tools_json = server.list_tools();
    auto tools = json::parse(tools_json);
    REQUIRE(tools.size() == 2);
}

// ── v2.3.10: cover required_access_level overrides ──

TEST_CASE("Diagnostics tools advertise READ access",
          "[diagnostics][v2.3.10][coverage]") {
    auto server = make_diagnostics_server();

    // The tools register names "diagnostics" and "check_errors" under
    // the "diagnostics" server. required_access_level fires when the
    // ServerManager queries access for a tool name.
    auto* tool_diag = server.registry().get_tool("diagnostics");
    auto* tool_check = server.registry().get_tool("check_errors");
    REQUIRE(tool_diag != nullptr);
    REQUIRE(tool_check != nullptr);
    CHECK(tool_diag->required_access_level()
          == entropic::MCPAccessLevel::READ);
    CHECK(tool_check->required_access_level()
          == entropic::MCPAccessLevel::READ);
}
