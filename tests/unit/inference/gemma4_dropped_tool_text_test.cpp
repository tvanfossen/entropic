// SPDX-License-Identifier: Apache-2.0
/**
 * @file gemma4_dropped_tool_text_test.cpp
 * @brief A Gemma 4 tool-call span the parser could not read is never lost.
 *
 * v2.14.6 gate (gh87-backend-common-chat, thinking on): the model wrote
 * `<|tool_call>call:read_file{path: "/etc/hostname"}<tool_call|>` with plain
 * quotes. At the pinned llama.cpp, common_chat_parse returned no call and
 * content holding only the thought block, which the reasoning strip then
 * emptied, so the call vanished without a log line. Upstream master still
 * requires `<|"|>` around string values, so there is no parse fix to mirror.
 * The invariant here is ours: unread call text stays visible.
 *
 * @version 2.14.6
 */

#include <catch2/catch_test_macros.hpp>

#include "gemma4_dropped_tool_text.h"

#include <string>

namespace {

const std::string kThought =
    "<|channel>thought\nI will generate the `read_file` tool call.<channel|>";
const std::string kPlainQuoteCall =
    "<|tool_call>call:read_file{path: \"/etc/hostname\"}<tool_call|>";

} // namespace

SCENARIO("v2.14.6: a Gemma 4 call span the parser dropped is restored",
         "[2.14.6][parse][gemma4]")
{
    GIVEN("the gate's emission: a thought, then a plain-quote call") {
        const std::string raw = kThought + kPlainQuoteCall;
        WHEN("the parse kept only the thought and found no call") {
            const auto spans = entropic::gemma4_dropped_tool_spans(raw, kThought);
            THEN("the unread call span is reported, verbatim") {
                REQUIRE(spans.size() == 1);
                CHECK(spans[0] == kPlainQuoteCall);
            }
            THEN("restoring it puts the call text back into content") {
                std::string content = kThought;
                entropic::restore_dropped_tool_spans(content, spans);
                CHECK(content.find(kPlainQuoteCall) != std::string::npos);
            }
        }
    }
    GIVEN("a call the parser did keep in content") {
        const std::string raw = "Sure. " + kPlainQuoteCall;
        THEN("nothing is reported twice") {
            CHECK(entropic::gemma4_dropped_tool_spans(raw, raw).empty());
        }
    }
    GIVEN("no tool-call markers at all") {
        THEN("nothing is reported") {
            CHECK(entropic::gemma4_dropped_tool_spans("hello", "hello").empty());
        }
    }
    GIVEN("a span cut off before its closing marker (max_tokens)") {
        const std::string cut = "<|tool_call>call:read_file{path: \"/etc";
        THEN("it is reported to the end of the text") {
            const auto spans = entropic::gemma4_dropped_tool_spans(cut, "");
            REQUIRE(spans.size() == 1);
            CHECK(spans[0] == cut);
        }
    }
    GIVEN("two calls, both dropped") {
        const std::string raw = kPlainQuoteCall + kPlainQuoteCall;
        THEN("both are reported") {
            CHECK(entropic::gemma4_dropped_tool_spans(raw, "").size() == 2);
        }
    }
}
