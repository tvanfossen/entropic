// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file context_inspect_test.cpp
 * @brief ContextInspectTool unit tests — context window inspection (P2-16).
 * @version 2.0.6-rc16
 */

#include <entropic/mcp/servers/entropic_server.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstring>
#include <string>

using namespace entropic;
using json = nlohmann::json;

// ── Mock provider ───────────────────────────────────────────────

/**
 * @brief Mock context history — returns [{role, content_preview, token_count_est}].
 *
 * Simulates sp_get_history output. Returns up to 5 entries; if max_entries > 0
 * caps at that value.
 *
 * @param max_entries Max messages (0 = all).
 * @param ud Unused.
 * @return JSON array string.
 * @internal
 * @version 2.0.6-rc16
 */
static char* mock_ctx_history(int max_entries, void* /*ud*/) {
    json arr = json::array();
    arr.push_back({{"role","system"},{"content_preview","You are helpful."},{"token_count_est",4}});
    arr.push_back({{"role","user"},{"content_preview","Hello"},{"token_count_est",1}});
    arr.push_back({{"role","assistant"},{"content_preview","Hi there!"},{"token_count_est",2}});
    arr.push_back({{"role","user"},{"content_preview","What time is it?"},{"token_count_est",4}});
    arr.push_back({{"role","assistant"},{"content_preview","I don't have a clock."},{"token_count_est",5}});

    if (max_entries > 0 && max_entries < static_cast<int>(arr.size())) {
        json trimmed = json::array();
        int start = static_cast<int>(arr.size()) - max_entries;
        for (int i = start; i < static_cast<int>(arr.size()); ++i) {
            trimmed.push_back(arr[i]);
        }
        return strdup(trimmed.dump().c_str());
    }
    return strdup(arr.dump().c_str());
}

/**
 * @brief Build provider wired only with get_history.
 * @return Partially populated provider.
 * @internal
 * @version 2.0.6-rc16
 */
static entropic_state_provider_t make_ctx_provider() {
    entropic_state_provider_t p{};
    p.get_history = mock_ctx_history;
    p.user_data   = nullptr;
    return p;
}

/**
 * @brief Extract result field from a serialized ServerResponse envelope.
 * @param envelope Serialized JSON envelope.
 * @return Result string.
 * @internal
 * @version 2.0.6-rc16
 */
static std::string get_result(const std::string& envelope) {
    return json::parse(envelope).at("result").get<std::string>();
}

// ── Tests ───────────────────────────────────────────────────────

TEST_CASE("context_inspect returns all messages when max_messages omitted",
          "[context_inspect][P2-16]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_ctx_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("context_inspect", R"({})"));
    auto j = json::parse(r);
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 5);
    REQUIRE(j[0]["role"] == "system");
}

TEST_CASE("context_inspect respects max_messages limit",
          "[context_inspect][P2-16]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_ctx_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("context_inspect", R"({"max_messages":2})"));
    auto j = json::parse(r);
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 2);
    REQUIRE(j[0]["role"] == "user");
    REQUIRE(j[1]["role"] == "assistant");
}

TEST_CASE("context_inspect entries have required fields",
          "[context_inspect][P2-16]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_ctx_provider();
    server.set_state_provider(prov);

    auto r = get_result(
        server.execute("context_inspect", R"({"max_messages":1})"));
    auto j = json::parse(r);
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 1);
    REQUIRE(j[0].contains("role"));
    REQUIRE(j[0].contains("content_preview"));
    REQUIRE(j[0].contains("token_count_est"));
}

TEST_CASE("context_inspect returns no directives",
          "[context_inspect][P2-16]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto prov = make_ctx_provider();
    server.set_state_provider(prov);

    auto envelope = server.execute(
        "context_inspect", R"({})");
    auto j = json::parse(envelope);
    REQUIRE(j["directives"].empty());
}

TEST_CASE("context_inspect without provider returns error",
          "[context_inspect][P2-16]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    // No set_state_provider call
    auto r = get_result(
        server.execute("context_inspect", R"({})"));
    REQUIRE(r.find("Error") != std::string::npos);
}
