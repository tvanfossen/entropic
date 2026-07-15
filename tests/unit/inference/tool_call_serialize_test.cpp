// SPDX-License-Identifier: Apache-2.0
/**
 * @file tool_call_serialize_test.cpp
 * @brief Unit tests for the shared (typed) tool-call serializer (gh#93).
 *
 * The serializer is the single source of truth used by BOTH production
 * (interface_factory.cpp) and the model-test harness (model_test_context.h).
 * Its defining property — and the one that closes the harness↔production
 * fidelity gap — is that each argument value is emitted in its NATURAL JSON
 * type when the value string is valid JSON, and as a JSON string otherwise.
 *
 * @version 2.8.0
 */

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "tool_call_serialize.h"

using entropic::serialize_tool_calls;
using entropic::ToolCall;
using nlohmann::json;

namespace {

ToolCall make_call(const std::string& name,
                   std::unordered_map<std::string, std::string> args) {
    ToolCall tc;
    tc.name = name;
    tc.arguments = std::move(args);
    return tc;
}

}  // namespace

TEST_CASE("serialize_tool_calls: empty input is an empty JSON array",
          "[inference][tool_call_serialize]") {
    REQUIRE(serialize_tool_calls({}) == "[]");
}

TEST_CASE("serialize_tool_calls: name is preserved and arguments is an object",
          "[inference][tool_call_serialize]") {
    auto out = json::parse(serialize_tool_calls(
        {make_call("filesystem.read_file", {{"path", "/tmp/x"}})}));
    REQUIRE(out.is_array());
    REQUIRE(out.size() == 1);
    CHECK(out[0]["name"] == "filesystem.read_file");
    REQUIRE(out[0]["arguments"].is_object());
    // Non-JSON string value stays a JSON string.
    CHECK(out[0]["arguments"]["path"].is_string());
    CHECK(out[0]["arguments"]["path"] == "/tmp/x");
}

TEST_CASE("serialize_tool_calls: JSON-valued argument strings keep their type",
          "[inference][tool_call_serialize]") {
    auto out = json::parse(serialize_tool_calls({make_call("t", {
        {"count", "42"},
        {"ratio", "0.5"},
        {"enabled", "true"},
        {"items", "[1,2,3]"},
        {"nested", R"({"a":1})"},
        {"nothing", "null"},
        {"label", "hello"},          // not JSON → stays a string
    })}));
    const auto& a = out[0]["arguments"];

    SECTION("integer") { CHECK(a["count"].is_number_integer()); CHECK(a["count"] == 42); }
    SECTION("float")   { CHECK(a["ratio"].is_number_float());   CHECK(a["ratio"] == 0.5); }
    SECTION("bool")    { CHECK(a["enabled"].is_boolean());      CHECK(a["enabled"] == true); }
    SECTION("array")   { CHECK(a["items"].is_array());          CHECK(a["items"].size() == 3); }
    SECTION("object")  { CHECK(a["nested"].is_object());        CHECK(a["nested"]["a"] == 1); }
    SECTION("null")    { CHECK(a["nothing"].is_null()); }
    SECTION("plain string stays a string") {
        CHECK(a["label"].is_string());
        CHECK(a["label"] == "hello");
    }
}

TEST_CASE("gh#118: serialize_tool_calls does not throw on MTP-split UTF-8 argument value",
          "[inference][tool_call_serialize][gh118][regression][2.9.16]") {
    // 0xE2 0x80 0x2E: incomplete 3-byte sequence (em-dash split at MTP draft
    // boundary). Without sanitization, nlohmann::json::dump() throws
    // type_error.316 inside serialize_tool_calls before the engine's
    // mcp::sanitize_utf8 guard at parse_tool_calls:1386 can run.
    std::string bad_payload = "temperature: 23\xE2\x80\x2E";  // split codepoint at index 17

    REQUIRE_NOTHROW(serialize_tool_calls(
        {make_call("mqtt_publish", {{"topic", "sensor/data"}, {"payload", bad_payload}})}));

    auto out = json::parse(serialize_tool_calls(
        {make_call("mqtt_publish", {{"topic", "sensor/data"}, {"payload", bad_payload}})}));
    REQUIRE(out.is_array());
    REQUIRE(out.size() == 1);
    // Argument values must be present; invalid bytes replaced by U+FFFD.
    CHECK(out[0]["arguments"]["topic"] == "sensor/data");
    CHECK(out[0]["arguments"]["payload"].get<std::string>().find('\xE2') == std::string::npos);
}

TEST_CASE("serialize_tool_calls: multiple calls preserve order",
          "[inference][tool_call_serialize]") {
    auto out = json::parse(serialize_tool_calls({
        make_call("first", {{"k", "1"}}),
        make_call("second", {{"k", "2"}}),
    }));
    REQUIRE(out.size() == 2);
    CHECK(out[0]["name"] == "first");
    CHECK(out[1]["name"] == "second");
}
