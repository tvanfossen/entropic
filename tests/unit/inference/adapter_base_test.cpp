// SPDX-License-Identifier: Apache-2.0
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

// ── v2.3.10 [adapter_base_topup] ──────────────────────────
// Targets: parse_bare_json_tool_calls (140-167), try_recover_json
// regex/full-miss (379-390, 413-417), format_tools default + malformed
// (422-446), format_content_parts image branches (497-517),
// format_system_prompt prefix extraction + malformed catch (130-141),
// parse_tagged_tool_calls warn branches (263-271).

namespace {

class FormatTestAdapter : public entropic::ChatAdapter {
public:
    FormatTestAdapter() : ChatAdapter("test", "id") {}
    std::string chat_format() const override { return "chatml"; }
    entropic::ParseResult parse_tool_calls(
        const std::string& content) const override {
        entropic::ParseResult r;
        r.tool_calls = parse_tagged_tool_calls(content);
        r.cleaned_content = strip_think_blocks(content);
        return r;
    }
    using ChatAdapter::parse_bare_json_tool_calls;
    using ChatAdapter::parse_tagged_tool_calls;
    using ChatAdapter::try_recover_json;
    using ChatAdapter::format_tools;
    using ChatAdapter::format_system_with_vision;
    using ChatAdapter::format_content_parts;
    using ChatAdapter::format_system_prompt;
    using ChatAdapter::format_tool_result;
};

}  // namespace

TEST_CASE("parse_bare_json_tool_calls handles skip/valid/alias-key lines",
          "[v2.3.10][inference][adapter_base_topup]")
{
    FormatTestAdapter adapter;
    // Lines: whitespace, non-brace, brace-no-name, valid, unparseable,
    // alias key — only the two well-formed name-bearing lines yield calls.
    std::string content =
        "   \n"
        "hello world\n"
        R"({"foo":1})" "\n"
        R"({"name":"fs.read","arguments":{"k":"v"}})" "\n"
        R"({"name":)" "\n"
        R"({"tool_name":"echo"})" "\n";

    auto calls = adapter.parse_bare_json_tool_calls(content);
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0].name == "fs.read");
    REQUIRE(calls[0].arguments.count("k") == 1);
    REQUIRE(calls[1].name == "echo");

    // Empty input → empty result.
    REQUIRE(adapter.parse_bare_json_tool_calls("").empty());
}

TEST_CASE("try_recover_json regex fallback + full-miss",
          "[v2.3.10][inference][adapter_base_topup]")
{
    FormatTestAdapter adapter;

    // Unparseable after fix-up + has `"name":"..."` substring → regex
    // fallback at lines 379-390 / 413-415 returns just-name ToolCall.
    auto r1 = adapter.try_recover_json(
        R"({"name":"fs.read", garbage missing braces)");
    REQUIRE(r1.has_value());
    REQUIRE(r1->name == "fs.read");
    REQUIRE(r1->arguments.empty());

    // No name substring → nullopt (line 416).
    REQUIRE_FALSE(adapter.try_recover_json("totally broken {{{").has_value());
}

TEST_CASE("format_tools default formatter shape + malformed catch",
          "[v2.3.10][inference][adapter_base_topup]")
{
    FormatTestAdapter adapter;
    std::vector<std::string> tools = {
        R"({"name":"fs.read","description":"R","inputSchema":{"type":"object"}})",
        R"({"name":"fs.write","description":"W","inputSchema":{}})",
        "this is not json at all"  // exercises catch at lines 444-446
    };
    auto out = adapter.format_tools(tools);
    REQUIRE(out.find("## Tools") != std::string::npos);
    REQUIRE(out.find("### fs.read") != std::string::npos);
    REQUIRE(out.find("### fs.write") != std::string::npos);
    REQUIRE(out.find("(malformed tool definition)") != std::string::npos);

    // Empty list → header only.
    auto out2 = adapter.format_tools({});
    REQUIRE(out2.find("## Tools") != std::string::npos);
    REQUIRE(out2.find("###") == std::string::npos);
}

TEST_CASE("format_system_prompt assembly + dotted-name prefix extraction",
          "[v2.3.10][inference][adapter_base_topup]")
{
    FormatTestAdapter adapter;
    std::vector<std::string> tools = {
        R"({"name":"fs.read","description":"r","inputSchema":{}})",
        R"({"name":"git.status","description":"g","inputSchema":{}})",
        R"({"name":"undotted","description":"u","inputSchema":{}})",
        "not valid json"  // exercises catch at line 138
    };
    auto prompt = adapter.format_system_prompt("base context", tools);
    REQUIRE(prompt.find("id") != std::string::npos);
    REQUIRE(prompt.find("base context") != std::string::npos);
    REQUIRE(prompt.find("fs.read") != std::string::npos);

    // No base + no tools → identity passes through unchanged.
    REQUIRE(adapter.format_system_prompt("", {}) == "id");
}

TEST_CASE("format_tool_result default user-message + content_parts shapes",
          "[v2.3.10][inference][adapter_base_topup]")
{
    FormatTestAdapter adapter;
    entropic::ToolCall tc;
    tc.id = "1"; tc.name = "fs.read";
    auto msg = adapter.format_tool_result(tc, "/etc/hosts");
    REQUIRE(msg.role == "user");
    REQUIRE(msg.content.find("fs.read") != std::string::npos);
    REQUIRE(msg.content.find("/etc/hosts") != std::string::npos);
    REQUIRE(msg.content.find("Continue") != std::string::npos);

    // format_content_parts: text + image_path + image_url branches.
    std::vector<entropic::ContentPart> parts;
    entropic::ContentPart txt; txt.type = entropic::ContentPartType::TEXT;
    txt.text = "describe this"; parts.push_back(txt);
    entropic::ContentPart ip; ip.type = entropic::ContentPartType::IMAGE;
    ip.image_path = "/tmp/img.png"; parts.push_back(ip);
    entropic::ContentPart iu; iu.type = entropic::ContentPartType::IMAGE;
    iu.image_url = "https://example.com/i.png"; parts.push_back(iu);
    auto js = adapter.format_content_parts(parts);
    REQUIRE(js.find("describe this") != std::string::npos);
    REQUIRE(js.find("/tmp/img.png") != std::string::npos);
    REQUIRE(js.find("https://example.com/i.png") != std::string::npos);
    REQUIRE(js.find("\"image\"") != std::string::npos);
    REQUIRE(adapter.format_content_parts({}) == "[]");

    // format_system_with_vision returns input unchanged.
    REQUIRE(adapter.format_system_with_vision("base", true) == "base");
    REQUIRE(adapter.format_system_with_vision("base", false) == "base");
}

TEST_CASE("parse_tagged_tool_calls warns on markup substring without match",
          "[v2.3.10][inference][adapter_base_topup]")
{
    FormatTestAdapter adapter;
    // <tool_call> substring + no close → warn branch (263-271).
    REQUIRE(adapter.parse_tagged_tool_calls(
        "noise <tool_call> but no close ever").empty());
    // <|im_start|>tool_call substring + no close → warn branch.
    REQUIRE(adapter.parse_tagged_tool_calls(
        "<|im_start|>tool_call missing close").empty());
    // Tagged block with unrecoverable malformed JSON → matched-but-failed warn.
    REQUIRE(adapter.parse_tagged_tool_calls(
        "<tool_call>}}}garbage no name{{{ </tool_call>").empty());
}
