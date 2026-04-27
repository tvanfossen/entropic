// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_inspect.cpp
 * @brief InspectTool unit tests with mock state provider.
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

// ── Mock state provider (shared with test_diagnose.cpp) ─────────

/**
 * @brief Mock config — returns object with model, loop, permissions.
 * @param ud Unused.
 * @return JSON config.
 * @internal
 * @version 1.9.12
 */
static char* mock_config(void* /*ud*/) {
    return strdup(R"({
        "model":{"path":"/test.gguf"},
        "loop":{"max_iterations":50},
        "permissions":{"auto_approve_tools":false}
    })");
}

/**
 * @brief Mock identities — array with name field for filtering.
 * @param ud Unused.
 * @return JSON identities.
 * @internal
 * @version 1.9.12
 */
static char* mock_identities(void* /*ud*/) {
    return strdup(R"([
        {"name":"lead"},
        {"name":"eng"}
    ])");
}

/**
 * @brief Mock tools — array with name field for filtering.
 * @param ud Unused.
 * @return JSON tools.
 * @internal
 * @version 1.9.12
 */
static char* mock_tools(void* /*ud*/) {
    return strdup(R"([
        {"name":"filesystem.read_file","description":"Read a file"},
        {"name":"entropic.todo","description":"Todo list"}
    ])");
}

/**
 * @brief Mock history — returns max_entries items.
 * @param max_entries Max entries.
 * @param ud Unused.
 * @return JSON history.
 * @internal
 * @version 1.9.12
 */
static char* mock_history(int max_entries, void* /*ud*/) {
    json arr = json::array();
    int count = (max_entries <= 0) ? 5 : max_entries;
    if (count > 5) { count = 5; }
    for (int i = 0; i < count; ++i) {
        arr.push_back({{"sequence", i}, {"tool_name", "test.t"}});
    }
    return strdup(arr.dump().c_str());
}

/**
 * @brief Mock state callback.
 * @param ud Unused.
 * @return JSON state.
 * @internal
 * @version 1.9.12
 */
static char* mock_state(void* /*ud*/) {
    return strdup(R"({
        "agent_state":"EXECUTING","delegation_depth":0,
        "active_phase":"default","iteration":5
    })");
}

/**
 * @brief Mock metrics callback.
 * @param ud Unused.
 * @return JSON metrics.
 * @internal
 * @version 1.9.12
 */
static char* mock_metrics(void* /*ud*/) {
    return strdup(R"({
        "total_iterations":5,"total_tool_calls":3,
        "total_tool_errors":0
    })");
}

/**
 * @brief Mock docs callback with section filtering.
 * @param section Section name.
 * @param ud Unused.
 * @return Documentation text.
 * @internal
 * @version 1.9.12
 */
static char* mock_docs(const char* section, void* /*ud*/) {
    if (section == nullptr) {
        return strdup("Full docs content here");
    }
    if (std::string(section) == "inference") {
        return strdup("InferenceBackend docs");
    }
    return strdup("");
}

/**
 * @brief Build mock provider for inspect tests.
 * @return Populated provider.
 * @internal
 * @version 1.9.12
 */
static entropic_state_provider_t make_provider() {
    entropic_state_provider_t p{};
    p.get_config = mock_config;
    p.get_identities = mock_identities;
    p.get_tools = mock_tools;
    p.get_history = mock_history;
    p.get_state = mock_state;
    p.get_metrics = mock_metrics;
    p.get_docs = mock_docs;
    p.user_data = nullptr;
    return p;
}

/**
 * @brief Extract result from envelope.
 * @param envelope JSON envelope string.
 * @return Result string.
 * @internal
 * @version 1.9.12
 */
static std::string get_result(const std::string& envelope) {
    return json::parse(envelope).at("result").get<std::string>();
}

// ── Tests ───────────────────────────────────────────────────────

TEST_CASE("test_inspect_config_full", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect", R"({"target":"config"})"));
    auto j = json::parse(r);
    REQUIRE(j.contains("model"));
    REQUIRE(j.contains("loop"));
}

TEST_CASE("test_inspect_config_section", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect",
            R"({"target":"config","key":"model"})"));
    auto j = json::parse(r);
    REQUIRE(j.contains("path"));
}

TEST_CASE("test_inspect_config_unknown_section", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect",
            R"({"target":"config","key":"nonexistent"})"));
    REQUIRE(r.find("Error") != std::string::npos);
    REQUIRE(r.find("not found") != std::string::npos);
}

TEST_CASE("test_inspect_identity_by_name", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect",
            R"({"target":"identity","key":"eng"})"));
    auto j = json::parse(r);
    REQUIRE(j["name"] == "eng");
}

TEST_CASE("test_inspect_identity_unknown", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect",
            R"({"target":"identity","key":"nonexistent"})"));
    REQUIRE(r.find("Error") != std::string::npos);
    REQUIRE(r.find("Available") != std::string::npos);
}

TEST_CASE("test_inspect_tool_by_name", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect",
            R"({"target":"tool","key":"filesystem.read_file"})"));
    auto j = json::parse(r);
    REQUIRE(j["name"] == "filesystem.read_file");
}

TEST_CASE("test_inspect_tool_unknown", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect",
            R"({"target":"tool","key":"nonexistent"})"));
    REQUIRE(r.find("Error") != std::string::npos);
}

TEST_CASE("test_inspect_state", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect", R"({"target":"state"})"));
    auto j = json::parse(r);
    REQUIRE(j.contains("agent_state"));
    REQUIRE(j.contains("delegation_depth"));
}

TEST_CASE("test_inspect_metrics", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect", R"({"target":"metrics"})"));
    auto j = json::parse(r);
    REQUIRE(j.contains("total_iterations"));
    REQUIRE(j.contains("total_tool_calls"));
}

TEST_CASE("test_inspect_history_default", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect", R"({"target":"history"})"));
    auto j = json::parse(r);
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 5);  // mock caps at 5
}

TEST_CASE("test_inspect_history_with_limit", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect",
            R"({"target":"history","key":"3"})"));
    auto j = json::parse(r);
    REQUIRE(j.size() == 3);
}

TEST_CASE("test_inspect_docs_full", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect", R"({"target":"docs"})"));
    REQUIRE(r.find("Full docs") != std::string::npos);
}

TEST_CASE("test_inspect_docs_section", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect",
            R"({"target":"docs","key":"inference"})"));
    REQUIRE(r.find("InferenceBackend") != std::string::npos);
}

TEST_CASE("test_inspect_unknown_target", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("inspect",
            R"({"target":"bogus"})"));
    REQUIRE(r.find("Error") != std::string::npos);
    REQUIRE(r.find("unknown target") != std::string::npos);
}

TEST_CASE("test_inspect_no_directives", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_provider();
    server.set_state_provider(prov);

    auto envelope = server.execute(
        "inspect", R"({"target":"state"})");
    auto j = json::parse(envelope);
    REQUIRE(j["directives"].empty());
}

TEST_CASE("test_inspect_no_provider_error", "[inspect]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto r = get_result(
        server.execute("inspect", R"({"target":"state"})"));
    REQUIRE(r.find("Error") != std::string::npos);
}
