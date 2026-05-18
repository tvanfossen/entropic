// SPDX-License-Identifier: Apache-2.0
/**
 * @file qwen36_adapter_test.cpp
 * @brief Tests for Qwen 3.6 XML tool call parsing (v2.1.9, gh#45).
 * @version 2.1.9
 */

#include <catch2/catch_test_macros.hpp>

#include "adapters/qwen36_adapter.h"

// ── XML function call parsing ──────────────────────────────

SCENARIO("Qwen36 XML function call parsing", "[qwen36][parsing]") {
    entropic::Qwen36Adapter adapter("eng", "test identity");

    GIVEN("content with XML function call") {
        std::string content =
            "<tool_call>\n"
            "<function=filesystem.read_file>\n"
            "<parameter=path>/src/main.cpp</parameter>\n"
            "</function>\n"
            "</tool_call>";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("one ToolCall with correct name and args") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "filesystem.read_file");
                REQUIRE(result.tool_calls[0].arguments.at("path") == "/src/main.cpp");
            }
        }
    }

    GIVEN("content with multiple XML function calls") {
        std::string content =
            "<tool_call>\n"
            "<function=fs.read><parameter=path>/a</parameter></function>\n"
            "</tool_call>\n"
            "<tool_call>\n"
            "<function=fs.write><parameter=path>/b</parameter></function>\n"
            "</tool_call>";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("two ToolCalls are returned in order") {
                REQUIRE(result.tool_calls.size() == 2);
                REQUIRE(result.tool_calls[0].name == "fs.read");
                REQUIRE(result.tool_calls[1].name == "fs.write");
            }
        }
    }

    GIVEN("malformed XML function call (missing closing tag)") {
        std::string content =
            "<tool_call>\n"
            "<function=fs.read>\n"
            "<parameter=path>/a";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("no calls are parsed (defensive)") {
                REQUIRE(result.tool_calls.empty());
            }
        }
    }
}

// ── Tool result formatting ─────────────────────────────────

SCENARIO("Qwen36 tool result formatting", "[qwen36][format]") {
    entropic::Qwen36Adapter adapter("eng", "test");

    GIVEN("a tool call and result") {
        entropic::ToolCall tc;
        tc.id = "1";
        tc.name = "fs.read";

        WHEN("format_tool_result is called") {
            auto msg = adapter.format_tool_result(tc, "file contents here");
            THEN("result is wrapped in <tool_response> tags") {
                REQUIRE(msg.role == "user");
                REQUIRE(msg.content.find("<tool_response>") != std::string::npos);
                REQUIRE(msg.content.find("</tool_response>") != std::string::npos);
                REQUIRE(msg.content.find("file contents here") != std::string::npos);
            }
        }
    }
}

// ── Fallback to tagged JSON ────────────────────────────────

SCENARIO("Qwen36 fallback to tagged JSON", "[qwen36][fallback]") {
    entropic::Qwen36Adapter adapter("eng", "test");

    GIVEN("content with tagged JSON (no XML function call)") {
        std::string content =
            R"(<tool_call>{"name":"fs.read","arguments":{"path":"/"}}</tool_call>)";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("tagged JSON parsing succeeds as fallback") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "fs.read");
            }
        }
    }
}

// ── Multi-turn / reasoning content cleaning ────────────────

SCENARIO("Qwen36 content cleaning across multi-turn output",
         "[qwen36][clean][reasoning]") {
    entropic::Qwen36Adapter adapter("eng", "test");

    GIVEN("content with think blocks and a tool call") {
        std::string content =
            "<think>plan: read the file then describe it</think>\n"
            "Here is the plan.\n"
            "<tool_call>\n"
            "<function=fs.read><parameter=path>/etc/hostname</parameter></function>\n"
            "</tool_call>";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("think + tool_call tags are stripped, narration preserved") {
                REQUIRE(result.cleaned_content.find("<think>") == std::string::npos);
                REQUIRE(result.cleaned_content.find("<tool_call>") == std::string::npos);
                REQUIRE(result.cleaned_content.find("Here is the plan") != std::string::npos);
            }
            AND_THEN("the tool call is still extracted") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "fs.read");
            }
        }
    }
}

// ── Vision content parts ───────────────────────────────────

SCENARIO("Qwen36 vision content-parts formatting", "[qwen36][vision]") {
    entropic::Qwen36Adapter adapter("eng", "test");

    GIVEN("a mixed text + image content part list") {
        std::vector<entropic::ContentPart> parts;
        entropic::ContentPart text;
        text.type = entropic::ContentPartType::TEXT;
        text.text = "What's in this image?";
        parts.push_back(std::move(text));

        entropic::ContentPart image;
        image.type = entropic::ContentPartType::IMAGE;
        image.image_path = "/tmp/example.png";
        parts.push_back(std::move(image));

        WHEN("format_content_parts is called") {
            auto json = adapter.format_content_parts(parts);
            THEN("output is a JSON array containing both parts") {
                REQUIRE(json.find("\"type\":\"text\"") != std::string::npos);
                REQUIRE(json.find("What's in this image?") != std::string::npos);
                REQUIRE(json.find("\"type\":\"image\"") != std::string::npos);
                REQUIRE(json.find("/tmp/example.png") != std::string::npos);
            }
        }
    }
}

// ── Vision system prompt ───────────────────────────────────

SCENARIO("Qwen36 vision system prompt extension", "[qwen36][vision]") {
    entropic::Qwen36Adapter adapter("eng", "test");

    WHEN("has_vision is false") {
        auto out = adapter.format_system_with_vision("You are helpful.", false);
        THEN("system prompt is unchanged") {
            REQUIRE(out == "You are helpful.");
        }
    }
    WHEN("has_vision is true") {
        auto out = adapter.format_system_with_vision("You are helpful.", true);
        THEN("vision instruction is appended") {
            REQUIRE(out.find("You are helpful.") != std::string::npos);
            REQUIRE(out.find("images") != std::string::npos);
        }
    }
}
