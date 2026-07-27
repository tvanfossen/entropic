// SPDX-License-Identifier: Apache-2.0
/**
 * @file thinking_markers_test.cpp
 * @brief gh#108: thinking-block removal must be resolved per model family.
 *
 * @par The defect these pin
 * Gemma-4 emits `<|channel>…<channel|>`; Qwen/Nemotron emit `<think>…</think>`.
 * Every family had an adapter that strips its own marker — except gemma4,
 * which has no adapter at all (`adapter_registry.cpp:56` — "gemma4 is
 * intentionally absent") and therefore resolves to `GenericAdapter`. Generic
 * strips `<think>` (`generic_adapter.cpp:34`), a marker gemma4 never emits,
 * and leaves `<|channel>` untouched.
 *
 * `strip_thinking_channels` — the only `<|channel>` handler — lives inside
 * `parse_response`, reachable only when `common_chat_parse_reliable()` is
 * true, which requires BOTH a tooled render AND `PEG_GEMMA4`. So a toolless
 * generate on a gemma4 tier leaks raw reasoning into content on every path:
 * plain, streaming, MTP, batch. A GPU model test
 * (test_gh108_mtp_stream_grammar.cpp) proves it end-to-end; these pin the
 * unit-level cause on CPU.
 *
 * @version 2.10.3
 */

#include <entropic/inference/adapters/adapter_base.h>
#include <entropic/core/stream_think_filter.h>

#include "adapters/gemma4_adapter.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

namespace entropic {
std::unique_ptr<ChatAdapter> create_adapter(const std::string& name,
                                            const std::string& tier_name,
                                            const std::string& identity_prompt);
}  // namespace entropic

using entropic::create_adapter;

namespace {

/// Gemma-4 QAT reasoning wrapper, as emitted live by gemma-4-E2B-it.
constexpr const char* kGemmaRaw =
    "<|channel>thought\nThinking Process: the user wants a greeting."
    "<channel|>Hello.";

/// Qwen / Nemotron reasoning wrapper.
constexpr const char* kQwenRaw =
    "<think>The user wants a greeting.</think>Hello.";

/**
 * @brief Collect filtered output from a StreamThinkFilter.
 * @version 2.10.3
 */
struct Sink {
    std::string out;  ///< Everything the consumer callback received

    /**
     * @brief C-style trampoline matching TokenCallback.
     * @param tok Token bytes.
     * @param len Token length.
     * @param ud Sink instance.
     * @version 2.10.3
     */
    static void cb(const char* tok, size_t len, void* ud) {
        static_cast<Sink*>(ud)->out.append(tok, len);
    }
};

}  // namespace

SCENARIO("gh#108 the adapter a gemma4 tier resolves to strips its own marker",
         "[inference][adapter][gh108][thinking]")
{
    GIVEN("the adapter created for adapter name 'gemma4'") {
        auto adapter = create_adapter("gemma4", "lead", "");
        REQUIRE(adapter != nullptr);

        WHEN("it parses a live gemma4 reasoning emission") {
            auto parsed = adapter->parse_tool_calls(kGemmaRaw);

            THEN("the reasoning block is removed from the content") {
                INFO("cleaned=[" << parsed.cleaned_content << "]");
                // RED before gh#108: gemma4 resolves to GenericAdapter, which
                // strips <think> and leaves <|channel> fully intact.
                CHECK(parsed.cleaned_content.find("<|channel")
                      == std::string::npos);
                CHECK(parsed.cleaned_content.find("Thinking Process")
                      == std::string::npos);
            }

            THEN("the actual answer survives") {
                CHECK(parsed.cleaned_content.find("Hello.")
                      != std::string::npos);
            }
        }
    }

    GIVEN("the qwen35 adapter, which already handles its own marker") {
        auto adapter = create_adapter("qwen35", "lead", "");
        REQUIRE(adapter != nullptr);

        WHEN("it parses a qwen reasoning emission") {
            auto parsed = adapter->parse_tool_calls(kQwenRaw);

            THEN("it still strips — the control that catches a regression "
                 "from re-pointing markers at the wrong family") {
                CHECK(parsed.cleaned_content.find("<think>")
                      == std::string::npos);
                CHECK(parsed.cleaned_content.find("Hello.")
                      != std::string::npos);
            }
        }
    }
}

SCENARIO("gh#108 the gemma4 adapter and the stream filter agree on markers",
         "[inference][adapter][gh108][thinking]")
{
    // The buffered strip and the live filter drifting apart is exactly how
    // v2.10.0 shipped a fix for the wrong marker. Single source of truth.
    GIVEN("the gemma4 adapter's declared markers") {
        auto m = entropic::Gemma4Adapter("lead", "").thinking_markers();

        THEN("they are the QAT channel pair, not the <think> default") {
            CHECK(m.open == "<|channel>");
            CHECK(m.close == "<channel|>");
        }

        THEN("a filter built from them suppresses what the adapter strips") {
            Sink sink;
            entropic::StreamThinkFilter filter(&Sink::cb, &sink, m.open, m.close);
            filter.on_token(kGemmaRaw, std::char_traits<char>::length(kGemmaRaw));
            filter.flush();

            auto buffered =
                entropic::Gemma4Adapter("lead", "").parse_tool_calls(kGemmaRaw);
            CHECK(sink.out == buffered.cleaned_content);
        }
    }
}

SCENARIO("gh#108 StreamThinkFilter suppresses the family's marker live",
         "[inference][streaming][gh108][thinking]")
{
    // This filter is the ONLY defense on the agent-loop streaming path:
    // ResponseGenerator::generate_streaming builds result.content from its own
    // token accumulator and discards whatever apply_adapter_parse produced
    // (response_generator.cpp:360). If the filter misses the marker, raw
    // reasoning lands in conversation history.

    GIVEN("a gemma4-configured filter fed a reasoning emission in one chunk") {
        Sink sink;
        auto m = entropic::Gemma4Adapter("lead", "").thinking_markers();
        entropic::StreamThinkFilter filter(&Sink::cb, &sink, m.open, m.close);

        WHEN("the whole emission is processed") {
            filter.on_token(kGemmaRaw, std::char_traits<char>::length(kGemmaRaw));
            filter.flush();

            THEN("the consumer never sees the reasoning") {
                INFO("streamed=[" << sink.out << "]");
                // RED before gh#108: the filter hardcodes <think>/</think>
                // (stream_think_filter.cpp:43-44) and passes <|channel>
                // through untouched.
                CHECK(sink.out.find("<|channel") == std::string::npos);
                CHECK(sink.out.find("Thinking Process") == std::string::npos);
            }

            THEN("the answer still reaches the consumer") {
                CHECK(sink.out.find("Hello.") != std::string::npos);
            }
        }
    }

    GIVEN("a filter fed the same emission split mid-marker") {
        // The reason a streaming filter exists at all: a marker can arrive
        // across two token boundaries. Nothing currently pins this for either
        // family.
        Sink sink;
        auto m = entropic::Gemma4Adapter("lead", "").thinking_markers();
        entropic::StreamThinkFilter filter(&Sink::cb, &sink, m.open, m.close);
        const std::string raw = kGemmaRaw;
        const std::size_t split = 5;  // inside "<|channel>"

        WHEN("it arrives as two chunks split inside the open marker") {
            filter.on_token(raw.data(), split);
            filter.on_token(raw.data() + split, raw.size() - split);
            filter.flush();

            THEN("the split does not defeat suppression") {
                INFO("streamed=[" << sink.out << "]");
                CHECK(sink.out.find("<|channel") == std::string::npos);
                CHECK(sink.out.find("Thinking Process") == std::string::npos);
                CHECK(sink.out.find("Hello.") != std::string::npos);
            }
        }
    }

    GIVEN("a filter fed a qwen emission split mid-marker") {
        Sink sink;
        entropic::StreamThinkFilter filter(&Sink::cb, &sink);
        const std::string raw = kQwenRaw;
        const std::size_t split = 3;  // inside "<think>"

        WHEN("it arrives as two chunks") {
            filter.on_token(raw.data(), split);
            filter.on_token(raw.data() + split, raw.size() - split);
            filter.flush();

            THEN("the pre-existing family keeps working — regression control") {
                CHECK(sink.out.find("<think>") == std::string::npos);
                CHECK(sink.out.find("Hello.") != std::string::npos);
            }
        }
    }
}
