// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_diagnose.cpp
 * @brief DiagnoseTool unit tests with mock state provider.
 * @version 1.9.12
 */

#include <entropic/mcp/servers/entropic_server.h>
#include <entropic/mcp/server_base.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

using namespace entropic;
using json = nlohmann::json;

// ── Mock state provider ─────────────────────────────────────────

/**
 * @brief Mock config callback.
 * @param ud Unused.
 * @return JSON config string.
 * @internal
 * @version 1.9.12
 */
static char* mock_get_config(void* /*ud*/) {
    return strdup(R"({
        "model":{"path":"/test.gguf","context_length":8192},
        "loop":{"max_iterations":50},
        "permissions":{"auto_approve_tools":false}
    })");
}

/**
 * @brief Mock identities callback.
 * @param ud Unused.
 * @return JSON identities array.
 * @internal
 * @version 1.9.12
 */
static char* mock_get_identities(void* /*ud*/) {
    return strdup(R"([
        {"name":"lead","routable":false},
        {"name":"eng","routable":true}
    ])");
}

/**
 * @brief Mock tools callback.
 * @param ud Unused.
 * @return JSON tools array.
 * @internal
 * @version 1.9.12
 */
static char* mock_get_tools(void* /*ud*/) {
    return strdup(R"([
        {"name":"filesystem.read_file","server":"filesystem"},
        {"name":"entropic.todo","server":"entropic"}
    ])");
}

/**
 * @brief Mock history callback.
 * @param max_entries Max entries to return.
 * @param ud Unused.
 * @return JSON history array.
 * @internal
 * @version 1.9.12
 */
static char* mock_get_history(int max_entries, void* /*ud*/) {
    json arr = json::array();
    int count = (max_entries <= 0) ? 3 : max_entries;
    if (count > 3) { count = 3; }
    for (int i = 0; i < count; ++i) {
        arr.push_back({{"sequence", i}, {"tool_name", "test.tool"},
                       {"status", "success"}});
    }
    return strdup(arr.dump().c_str());
}

/**
 * @brief Mock state callback.
 * @param ud Unused.
 * @return JSON engine state.
 * @internal
 * @version 1.9.12
 */
static char* mock_get_state(void* /*ud*/) {
    return strdup(R"({
        "agent_state":"EXECUTING","delegation_depth":0,
        "active_phase":"default","iteration":7
    })");
}

/**
 * @brief Mock metrics callback.
 * @param ud Unused.
 * @return JSON metrics.
 * @internal
 * @version 1.9.12
 */
static char* mock_get_metrics(void* /*ud*/) {
    return strdup(R"({
        "total_iterations":7,"total_tool_calls":5,
        "total_tool_errors":1
    })");
}

/**
 * @brief Mock docs callback.
 * @param section Section name (nullptr for full).
 * @param ud Unused.
 * @return Documentation text.
 * @internal
 * @version 1.9.12
 */
static char* mock_get_docs(const char* section, void* /*ud*/) {
    if (section == nullptr) {
        return strdup("Full architecture reference text");
    }
    return strdup(("Section: " + std::string(section)).c_str());
}

/**
 * @brief Build a mock state provider.
 * @return Populated entropic_state_provider_t.
 * @internal
 * @version 1.9.12
 */
static entropic_state_provider_t make_mock_provider() {
    entropic_state_provider_t p{};
    p.get_config = mock_get_config;
    p.get_identities = mock_get_identities;
    p.get_tools = mock_get_tools;
    p.get_history = mock_get_history;
    p.get_state = mock_get_state;
    p.get_metrics = mock_get_metrics;
    p.get_docs = mock_get_docs;
    p.user_data = nullptr;
    return p;
}

/**
 * @brief Extract the "result" field from a ServerResponse JSON.
 * @param envelope Raw JSON string.
 * @return Result string.
 * @internal
 * @version 1.9.12
 */
static std::string extract_result(const std::string& envelope) {
    return json::parse(envelope).at("result").get<std::string>();
}

// ── Tests ───────────────────────────────────────────────────────

TEST_CASE("test_diagnose_returns_all_sections", "[diagnose]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto provider = make_mock_provider();
    server.set_state_provider(provider);

    auto envelope = server.execute("diagnose", "{}");
    auto result = json::parse(extract_result(envelope));

    REQUIRE(result.contains("engine"));
    REQUIRE(result.contains("config"));
    REQUIRE(result.contains("identities"));
    REQUIRE(result.contains("tools"));
    REQUIRE(result.contains("history"));
    REQUIRE(result.contains("metrics"));
    REQUIRE(result.contains("docs"));
}

TEST_CASE("test_diagnose_no_docs_by_default", "[diagnose]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto provider = make_mock_provider();
    server.set_state_provider(provider);

    auto envelope = server.execute("diagnose", "{}");
    auto result = json::parse(extract_result(envelope));

    REQUIRE(result["docs"].is_null());
}

TEST_CASE("test_diagnose_with_docs", "[diagnose]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto provider = make_mock_provider();
    server.set_state_provider(provider);

    auto envelope = server.execute(
        "diagnose", R"({"include_docs":true})");
    auto result = json::parse(extract_result(envelope));

    REQUIRE(result["docs"].is_string());
    REQUIRE(result["docs"].get<std::string>().find("architecture")
            != std::string::npos);
}

TEST_CASE("test_diagnose_history_limit_custom", "[diagnose]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto provider = make_mock_provider();
    server.set_state_provider(provider);

    auto envelope = server.execute(
        "diagnose", R"({"history_limit":2})");
    auto result = json::parse(extract_result(envelope));

    REQUIRE(result["history"].size() == 2);
}

TEST_CASE("test_diagnose_no_directives", "[diagnose]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto provider = make_mock_provider();
    server.set_state_provider(provider);

    auto envelope = server.execute("diagnose", "{}");
    auto j = json::parse(envelope);
    REQUIRE(j["directives"].empty());
}

TEST_CASE("test_diagnose_valid_json", "[diagnose]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto provider = make_mock_provider();
    server.set_state_provider(provider);

    auto envelope = server.execute("diagnose", "{}");
    auto result_str = extract_result(envelope);
    // Should not throw
    auto result = json::parse(result_str);
    REQUIRE(result.is_object());
}

TEST_CASE("test_diagnose_config_section", "[diagnose]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto provider = make_mock_provider();
    server.set_state_provider(provider);

    auto envelope = server.execute("diagnose", "{}");
    auto result = json::parse(extract_result(envelope));

    REQUIRE(result["config"].contains("model"));
    REQUIRE(result["config"].contains("loop"));
    REQUIRE(result["config"].contains("permissions"));
}

TEST_CASE("test_diagnose_metrics_fields", "[diagnose]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto provider = make_mock_provider();
    server.set_state_provider(provider);

    auto envelope = server.execute("diagnose", "{}");
    auto result = json::parse(extract_result(envelope));

    REQUIRE(result["metrics"].contains("total_iterations"));
    REQUIRE(result["metrics"].contains("total_tool_calls"));
    REQUIRE(result["metrics"].contains("total_tool_errors"));
}

TEST_CASE("test_diagnose_no_provider_error", "[diagnose]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    // Don't set provider
    auto envelope = server.execute("diagnose", "{}");
    auto result = extract_result(envelope);
    REQUIRE(result.find("Error") != std::string::npos);
}
