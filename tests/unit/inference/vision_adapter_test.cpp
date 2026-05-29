// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_vision_adapter.cpp
 * @brief Tests for ChatAdapter base-class vision / content-part formatting.
 * @version 2.7.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/adapters/adapter_base.h>

#include <nlohmann/json.hpp>

namespace {

/**
 * @brief Concrete test adapter for base class vision methods.
 * @version 1.9.11
 */
class TestAdapter : public entropic::ChatAdapter {
public:
    TestAdapter() : ChatAdapter("test", "test identity") {}

    std::string chat_format() const override { return "chatml"; }

    entropic::ParseResult parse_tool_calls(
        const std::string& content) const override {
        entropic::ParseResult r;
        r.cleaned_content = content;
        return r;
    }
};

} // anonymous namespace

// ── Base ChatAdapter vision defaults ────────────────────────

TEST_CASE("Base adapter returns system prompt unchanged with vision=false",
          "[adapter][vision]") {
    TestAdapter adapter;
    auto result = adapter.format_system_with_vision(
        "You are helpful.", false);
    REQUIRE(result == "You are helpful.");
}

TEST_CASE("Base adapter returns system prompt unchanged with vision=true",
          "[adapter][vision]") {
    TestAdapter adapter;
    auto result = adapter.format_system_with_vision(
        "You are helpful.", true);
    REQUIRE(result == "You are helpful.");
}

TEST_CASE("Base adapter format_content_parts produces OpenAI JSON",
          "[adapter][vision]") {
    TestAdapter adapter;

    std::vector<entropic::ContentPart> parts;

    entropic::ContentPart text;
    text.type = entropic::ContentPartType::TEXT;
    text.text = "hello";
    parts.push_back(text);

    entropic::ContentPart img;
    img.type = entropic::ContentPartType::IMAGE;
    img.image_path = "/tmp/a.png";
    parts.push_back(img);

    auto json_str = adapter.format_content_parts(parts);
    auto arr = nlohmann::json::parse(json_str);

    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 2);
    REQUIRE(arr[0]["type"] == "text");
    REQUIRE(arr[0]["text"] == "hello");
    REQUIRE(arr[1]["type"] == "image");
    REQUIRE(arr[1]["path"] == "/tmp/a.png");
}

// ── Content-part formatting (base default) ──────────────────
// gh#87 (v2.7.0): the per-family adapters were retired. Their vision
// overrides were dead (no production caller of format_system_with_vision /
// format_content_parts) and qwen35's were base delegates anyway, so the
// base-default coverage above plus this content-part check are sufficient.

TEST_CASE("format_content_parts yields the base OpenAI content array",
          "[adapter][vision]") {
    TestAdapter adapter;

    std::vector<entropic::ContentPart> parts;

    entropic::ContentPart text;
    text.type = entropic::ContentPartType::TEXT;
    text.text = "describe";
    parts.push_back(text);

    auto json_str = adapter.format_content_parts(parts);
    auto arr = nlohmann::json::parse(json_str);

    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 1);
    REQUIRE(arr[0]["type"] == "text");
}
