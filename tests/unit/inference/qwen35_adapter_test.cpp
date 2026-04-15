// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_qwen35_adapter.cpp
 * @brief Tests for Qwen 3.5 XML tool call parsing.
 * @version 1.8.2
 */

#include <catch2/catch_test_macros.hpp>

// Include the impl header directly (internal to .so)
#include "adapters/qwen35_adapter.h"

// ── XML function call parsing ──────────────────────────────

SCENARIO("XML function call parsing", "[qwen35][parsing]") {
    entropic::Qwen35Adapter adapter("eng", "test identity");

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
            THEN("two ToolCalls are returned") {
                REQUIRE(result.tool_calls.size() == 2);
                REQUIRE(result.tool_calls[0].name == "fs.read");
                REQUIRE(result.tool_calls[1].name == "fs.write");
            }
        }
    }
}

// ── Tool result formatting ─────────────────────────────────

SCENARIO("Qwen35 tool result formatting", "[qwen35][format]") {
    entropic::Qwen35Adapter adapter("eng", "test");

    GIVEN("a tool call and result") {
        entropic::ToolCall tc;
        tc.id = "1";
        tc.name = "fs.read";

        WHEN("format_tool_result is called") {
            auto msg = adapter.format_tool_result(tc, "file contents here");
            THEN("result is wrapped in tool_response tags") {
                REQUIRE(msg.role == "user");
                REQUIRE(msg.content.find("<tool_response>") != std::string::npos);
                REQUIRE(msg.content.find("</tool_response>") != std::string::npos);
                REQUIRE(msg.content.find("file contents here") != std::string::npos);
            }
        }
    }
}

// ── Fallback to tagged JSON ────────────────────────────────

SCENARIO("Qwen35 fallback to tagged JSON", "[qwen35][fallback]") {
    entropic::Qwen35Adapter adapter("eng", "test");

    GIVEN("content with tagged JSON (no XML)") {
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

// ── Content cleaning ───────────────────────────────────────

SCENARIO("Qwen35 content cleaning", "[qwen35][clean]") {
    entropic::Qwen35Adapter adapter("eng", "test");

    GIVEN("content with tool calls and think blocks") {
        std::string content =
            "<think>reasoning</think>\n"
            "Here is the answer.\n"
            "<tool_call>\n"
            "<function=fs.read><parameter=path>/</parameter></function>\n"
            "</tool_call>";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("cleaned content has no tags") {
                REQUIRE(result.cleaned_content.find("<think>") == std::string::npos);
                REQUIRE(result.cleaned_content.find("<tool_call>") == std::string::npos);
                REQUIRE(result.cleaned_content.find("Here is the answer") != std::string::npos);
            }
        }
    }
}
