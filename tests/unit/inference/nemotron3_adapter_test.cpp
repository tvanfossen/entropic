// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file nemotron3_adapter_test.cpp
 * @brief Tests for Nemotron 3 adapter (v2.1.9, gh#47).
 *
 * Nemotron 3 uses the qwen3_coder XML tool-call format and emits
 * `<think>...</think>` blocks for reasoning traces (separate special
 * tokens at the GGUF level; surfaced as inline text after detokenisation).
 *
 * @version 2.1.9
 */

#include <catch2/catch_test_macros.hpp>

#include "adapters/nemotron3_adapter.h"

// ── XML function call parsing ──────────────────────────────

SCENARIO("Nemotron3 XML function call parsing", "[nemotron3][parsing]") {
    entropic::Nemotron3Adapter adapter("eng", "test identity");

    GIVEN("content with a single XML function call") {
        std::string content =
            "<tool_call>\n"
            "<function=net.fetch>\n"
            "<parameter=url>https://example.com</parameter>\n"
            "</function>\n"
            "</tool_call>";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("name and argument are extracted") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "net.fetch");
                REQUIRE(result.tool_calls[0].arguments.at("url") == "https://example.com");
            }
        }
    }
}

// ── Reasoning trace handling ───────────────────────────────

SCENARIO("Nemotron3 reasoning-trace stripping", "[nemotron3][reasoning]") {
    entropic::Nemotron3Adapter adapter("eng", "test");

    GIVEN("content with multi-line <think> block and visible answer") {
        std::string content =
            "<think>\n"
            "step 1: parse the prompt\n"
            "step 2: choose a tool\n"
            "</think>\n"
            "The answer is 42.";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("think block is stripped, answer preserved") {
                REQUIRE(result.cleaned_content.find("<think>") == std::string::npos);
                REQUIRE(result.cleaned_content.find("step 1") == std::string::npos);
                REQUIRE(result.cleaned_content.find("The answer is 42.") != std::string::npos);
            }
            AND_THEN("no tool calls were emitted") {
                REQUIRE(result.tool_calls.empty());
            }
        }
    }

    GIVEN("interleaved think + tool call (reasoning then action)") {
        std::string content =
            "<think>I should look up the file first.</think>\n"
            "<tool_call>\n"
            "<function=fs.read><parameter=path>/tmp/x</parameter></function>\n"
            "</tool_call>";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("both the think strip and the tool extraction succeed") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "fs.read");
                REQUIRE(result.cleaned_content.find("<think>") == std::string::npos);
                REQUIRE(result.cleaned_content.find("<tool_call>") == std::string::npos);
            }
        }
    }
}

// ── Fallback path (qwen3_coder format absent) ──────────────

SCENARIO("Nemotron3 fallback to tagged JSON", "[nemotron3][fallback]") {
    entropic::Nemotron3Adapter adapter("eng", "test");

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

// ── Tool result formatting ─────────────────────────────────

SCENARIO("Nemotron3 tool result wraps in tool_response tags",
         "[nemotron3][format]") {
    entropic::Nemotron3Adapter adapter("eng", "test");
    entropic::ToolCall tc;
    tc.id = "1";
    tc.name = "fs.read";

    auto msg = adapter.format_tool_result(tc, "abcdef");
    REQUIRE(msg.role == "user");
    REQUIRE(msg.content.find("<tool_response>") != std::string::npos);
    REQUIRE(msg.content.find("abcdef") != std::string::npos);
}

// ── Chat format identifier ─────────────────────────────────

SCENARIO("Nemotron3 chat format is GGUF-embedded", "[nemotron3][format]") {
    entropic::Nemotron3Adapter adapter("eng", "test");
    REQUIRE(adapter.chat_format() == "");
}
