// SPDX-License-Identifier: Apache-2.0
/**
 * @file gemma4_adapter_test.cpp
 * @brief Tests for Gemma 4 adapter (v2.1.9, gh#46).
 *
 * Gemma 4's native tool-call output syntax is not yet authoritatively
 * documented; the adapter uses a permissive parser (tagged JSON, then
 * bare JSON). These tests exercise that contract — once the model-test
 * phase confirms the actual format, extend these cases with model-shaped
 * fixtures.
 *
 * @version 2.1.9
 */

#include <catch2/catch_test_macros.hpp>

#include "adapters/gemma4_adapter.h"

// ── gh#65: consumer-reported content shapes ────────────────

SCENARIO("Gemma4 parses the exact gh#65 content shape",
         "[gemma4][gh65][parsing]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("the exact <tool_call>{json}</tool_call> from the gh#65 report") {
        // Pulled verbatim from the issue body. The model emits this on
        // stdin; the consumer reports tool_calls stayed 0.
        std::string content =
            R"(<tool_call>{"name": "entropic.delegate", "arguments": )"
            R"({"target": "registrar", "task": "List all configured )"
            R"(subjects for this family."}}</tool_call>)";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);

            THEN("one tool call is extracted with the right name") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
            }

            THEN("the target argument round-trips") {
                REQUIRE(result.tool_calls.size() == 1);
                auto it = result.tool_calls[0].arguments.find("target");
                REQUIRE(it != result.tool_calls[0].arguments.end());
                // arguments[k] is the dump of the JSON value, so a
                // string value comes back quoted.
                REQUIRE(it->second.find("registrar") != std::string::npos);
            }

            THEN("the cleaned content drops the tool_call markup") {
                REQUIRE(result.cleaned_content.find("<tool_call>")
                        == std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 still parses tool_call inside a think block",
         "[gemma4][gh65][parsing]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("a model that emits the tool_call inside a <think> block") {
        // gh#65 hypothesis #4 — if think-stripping ran BEFORE parse,
        // this would silently lose the tool call. Current Gemma4Adapter
        // parses on raw content first; this test pins that behavior so
        // a future refactor doesn't regress.
        std::string content =
            "<think>I should call entropic.delegate now.\n"
            R"(<tool_call>{"name": "entropic.delegate", "arguments": )"
            R"({"target": "registrar", "task": "Q"}}</tool_call>)"
            "</think>"
            "Some assistant reply text after thinking.";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);

            THEN("the tool call is still extracted (parse runs before strip)") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
            }

            THEN("the cleaned content drops both the think block and the call") {
                REQUIRE(result.cleaned_content.find("<think>")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("<tool_call>")
                        == std::string::npos);
                // The post-think assistant text survives.
                REQUIRE(result.cleaned_content.find(
                    "Some assistant reply text") != std::string::npos);
            }
        }
    }
}

// ── Tagged JSON parsing ────────────────────────────────────

SCENARIO("Gemma4 tagged JSON tool call parsing", "[gemma4][parsing]") {
    entropic::Gemma4Adapter adapter("eng", "test identity");

    GIVEN("a well-formed tagged JSON call") {
        std::string content =
            R"(<tool_call>{"name":"fs.read","arguments":{"path":"/etc/hostname"}}</tool_call>)";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("the call is parsed and tag is stripped from content") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "fs.read");
                REQUIRE(result.cleaned_content.find("<tool_call>") == std::string::npos);
            }
        }
    }

    GIVEN("a malformed tagged JSON call (trailing comma)") {
        std::string content =
            R"(<tool_call>{"name":"fs.read","arguments":{"path":"/",}}</tool_call>)";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("recovery extracts the call") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "fs.read");
            }
        }
    }
}

// ── Bare JSON fallback ─────────────────────────────────────

SCENARIO("Gemma4 bare JSON fallback", "[gemma4][fallback]") {
    entropic::Gemma4Adapter adapter("eng", "test");

    GIVEN("content with a bare JSON line and no wrapper tags") {
        std::string content =
            "Here is what I'll do:\n"
            R"({"name":"fs.read","arguments":{"path":"/tmp/x"}})";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("the bare JSON call is extracted") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "fs.read");
            }
        }
    }
}

// ── Think-block stripping ──────────────────────────────────

SCENARIO("Gemma4 strips think blocks from content", "[gemma4][reasoning]") {
    entropic::Gemma4Adapter adapter("eng", "test");

    GIVEN("content with a think block and a visible answer") {
        std::string content =
            "<think>internal reasoning</think>\n"
            "The capital of France is Paris.";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("think is stripped, answer remains") {
                REQUIRE(result.cleaned_content.find("<think>") == std::string::npos);
                REQUIRE(result.cleaned_content.find("Paris") != std::string::npos);
            }
        }
    }
}

// ── Chat format / no-op tool result ────────────────────────

SCENARIO("Gemma4 reports GGUF-embedded chat format", "[gemma4][format]") {
    entropic::Gemma4Adapter adapter("eng", "test");
    REQUIRE(adapter.chat_format() == "");
}

SCENARIO("Gemma4 tool result uses base-class default formatting",
         "[gemma4][format]") {
    entropic::Gemma4Adapter adapter("eng", "test");

    entropic::ToolCall tc;
    tc.id = "1";
    tc.name = "fs.read";

    auto msg = adapter.format_tool_result(tc, "contents");
    REQUIRE(msg.role == "user");
    REQUIRE(msg.content.find("fs.read") != std::string::npos);
    REQUIRE(msg.content.find("contents") != std::string::npos);
}
