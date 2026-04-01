/**
 * @file test_adapter_base.cpp
 * @brief Tests for ChatAdapter base class parsing primitives.
 * @version 1.8.2
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/adapters/adapter_base.h>

namespace {

/**
 * @brief Concrete test adapter exposing base class primitives.
 * @version 1.8.2
 */
class TestAdapter : public entropic::ChatAdapter {
public:
    TestAdapter() : ChatAdapter("test", "test identity") {}

    std::string chat_format() const override { return "chatml"; }

    entropic::ParseResult parse_tool_calls(const std::string& content) const override {
        entropic::ParseResult r;
        r.tool_calls = parse_tagged_tool_calls(content);
        r.cleaned_content = strip_think_blocks(content);
        return r;
    }

    // Expose protected methods for testing
    using ChatAdapter::parse_tagged_tool_calls;
    using ChatAdapter::parse_bare_json_tool_calls;
    using ChatAdapter::extract_thinking;
    using ChatAdapter::strip_think_blocks;
    using ChatAdapter::try_recover_json;
};

} // anonymous namespace

// ── Tagged tool call parsing ───────────────────────────────

SCENARIO("Tagged tool call parsing", "[adapter][parsing]") {
    TestAdapter adapter;

    GIVEN("content with a tagged tool call") {
        std::string content =
            R"(<tool_call>{"name":"fs.read","arguments":{"path":"/"}}</tool_call>)";

        WHEN("parse_tagged_tool_calls is called") {
            auto calls = adapter.parse_tagged_tool_calls(content);
            THEN("one ToolCall is returned") {
                REQUIRE(calls.size() == 1);
                REQUIRE(calls[0].name == "fs.read");
                REQUIRE(calls[0].arguments.count("path") == 1);
            }
        }
    }

    GIVEN("content with multiple tagged tool calls") {
        std::string content =
            R"(<tool_call>{"name":"fs.read","arguments":{"path":"/a"}}</tool_call>)"
            R"(<tool_call>{"name":"fs.write","arguments":{"path":"/b"}}</tool_call>)";

        WHEN("parse_tagged_tool_calls is called") {
            auto calls = adapter.parse_tagged_tool_calls(content);
            THEN("two ToolCalls are returned") {
                REQUIRE(calls.size() == 2);
                REQUIRE(calls[0].name == "fs.read");
                REQUIRE(calls[1].name == "fs.write");
            }
        }
    }
}

// ── Think block handling ───────────────────────────────────

SCENARIO("Think block handling", "[adapter][think]") {
    TestAdapter adapter;

    GIVEN("content with think block") {
        std::string content = "<think>reasoning here</think>The answer is 42";

        WHEN("strip_think_blocks is called") {
            auto result = adapter.strip_think_blocks(content);
            THEN("think block is removed") {
                REQUIRE(result == "The answer is 42");
            }
        }

        WHEN("extract_thinking is called") {
            auto thinking = adapter.extract_thinking(content);
            THEN("thinking content is extracted") {
                REQUIRE(thinking == "reasoning here");
            }
        }
    }

    GIVEN("content with only think block") {
        std::string content = "<think>just thinking</think>";

        WHEN("strip_think_blocks is called") {
            auto result = adapter.strip_think_blocks(content);
            THEN("result is empty") {
                REQUIRE(result.empty());
            }
        }
    }
}

// ── Response completeness ──────────────────────────────────

SCENARIO("Response completeness detection", "[adapter][complete]") {
    TestAdapter adapter;

    GIVEN("content with only think block and no tool calls") {
        std::string content = "<think>thinking...</think>";

        WHEN("is_response_complete is called") {
            bool complete = adapter.is_response_complete(content, {});
            THEN("returns false (think-only = still working)") {
                REQUIRE_FALSE(complete);
            }
        }
    }

    GIVEN("content with text after think block") {
        std::string content = "<think>done thinking</think>Here is my answer.";

        WHEN("is_response_complete is called") {
            bool complete = adapter.is_response_complete(content, {});
            THEN("returns true") {
                REQUIRE(complete);
            }
        }
    }

    GIVEN("content with unclosed think block") {
        std::string content = "<think>still going...";

        WHEN("is_response_complete is called") {
            bool complete = adapter.is_response_complete(content, {});
            THEN("returns false") {
                REQUIRE_FALSE(complete);
            }
        }
    }

    GIVEN("content with tool calls") {
        entropic::ToolCall tc;
        tc.id = "1";
        tc.name = "fs.read";

        WHEN("is_response_complete is called") {
            bool complete = adapter.is_response_complete("text", {tc});
            THEN("returns false (has tool calls)") {
                REQUIRE_FALSE(complete);
            }
        }
    }
}

// ── JSON recovery ──────────────────────────────────────────

SCENARIO("Malformed JSON recovery", "[adapter][recovery]") {
    TestAdapter adapter;

    GIVEN("JSON with trailing comma") {
        std::string json = R"({"name": "fs.read", "arguments": {"path": "/",}})";

        WHEN("try_recover_json is called") {
            auto result = adapter.try_recover_json(json);
            THEN("ToolCall is recovered") {
                REQUIRE(result.has_value());
                REQUIRE(result->name == "fs.read");
            }
        }
    }

    GIVEN("JSON with single quotes") {
        std::string json = R"({'name': 'fs.read', 'arguments': {}})";

        WHEN("try_recover_json is called") {
            auto result = adapter.try_recover_json(json);
            THEN("ToolCall is recovered") {
                REQUIRE(result.has_value());
                REQUIRE(result->name == "fs.read");
            }
        }
    }
}
