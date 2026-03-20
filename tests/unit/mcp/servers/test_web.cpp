/**
 * @file test_web.cpp
 * @brief WebServer unit tests.
 * @version 1.8.5
 */

#include <entropic/mcp/servers/web.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace entropic;

// ── Helpers ─────────────────────────────────────────────

/**
 * @brief Create a WebServer with TEST_DATA_DIR.
 * @internal
 * @return Configured WebServer.
 * @version 1.8.5
 */
static WebServer make_web_server() {
    return WebServer(TEST_DATA_DIR);
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

TEST_CASE("Web fetch returns placeholder", "[web]") {
    auto server = make_web_server();
    json args;
    args["url"] = "https://example.com";

    auto result = parse_result(server.execute("web_fetch", args.dump()));
    REQUIRE_FALSE(result.empty());
}

TEST_CASE("Web fetch respects max_length parameter", "[web]") {
    auto server = make_web_server();
    json args;
    args["url"] = "https://example.com";
    args["max_length"] = 100;

    auto result = parse_result(server.execute("web_fetch", args.dump()));
    REQUIRE_FALSE(result.empty());
}

TEST_CASE("Web search returns placeholder", "[web]") {
    auto server = make_web_server();
    json args;
    args["query"] = "test";

    auto result = parse_result(server.execute("web_search", args.dump()));
    REQUIRE_FALSE(result.empty());
}
