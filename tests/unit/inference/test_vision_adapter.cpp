/**
 * @file test_vision_adapter.cpp
 * @brief Tests for ChatAdapter / Qwen35Adapter vision formatting.
 * @version 1.9.11
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/adapters/adapter_base.h>

#include "adapters/qwen35_adapter.h"

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

// ── Qwen35Adapter vision overrides ──────────────────────────

TEST_CASE("Qwen35 adapter appends vision context when has_vision=true",
          "[qwen35][vision]") {
    entropic::Qwen35Adapter adapter("test", "test identity");
    auto result = adapter.format_system_with_vision(
        "You are helpful.", true);

    REQUIRE(result.find("You are helpful.") != std::string::npos);
    REQUIRE(result.find("image") != std::string::npos);
    REQUIRE(result.size() > std::string("You are helpful.").size());
}

TEST_CASE("Qwen35 adapter unchanged when has_vision=false",
          "[qwen35][vision]") {
    entropic::Qwen35Adapter adapter("test", "test identity");
    auto result = adapter.format_system_with_vision(
        "You are helpful.", false);
    REQUIRE(result == "You are helpful.");
}

TEST_CASE("Qwen35 format_content_parts matches base OpenAI format",
          "[qwen35][vision]") {
    entropic::Qwen35Adapter adapter("test", "test identity");

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
