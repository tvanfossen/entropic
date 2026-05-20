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

SCENARIO("Gemma4 parses asymmetric <|tool_call>...</tool_call> "
         "(gh#65 v2.3.3 consumer repro)",
         "[gemma4][gh65][parsing]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("the asymmetric tag the consumer captured from gemma-4-E4B-it-Q8_0") {
        // Verbatim from gh#65 v2.3.3 follow-up comment. Open tag has
        // a leading `<|` pipe prefix (the Gemma 4 special token
        // surface form under llama.cpp's current pin); close tag is
        // plain `</tool_call>`. Pre-v2.3.3 this matched 0 calls and
        // the engine looped on the no-tool-call retry banner.
        std::string content =
            R"(<|tool_call>{"name": "entropic.delegate", )"
            R"("arguments": {"target": "curriculum", )"
            R"("task": "List all existing classes or lessons available in the system."}})"
            R"(</tool_call>)";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);

            THEN("one delegate call is extracted") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
                auto it = result.tool_calls[0].arguments.find("target");
                REQUIRE(it != result.tool_calls[0].arguments.end());
                REQUIRE(it->second.find("curriculum") != std::string::npos);
            }

            THEN("the cleaned content strips the asymmetric markup") {
                REQUIRE(result.cleaned_content.find("<|tool_call")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("</tool_call>")
                        == std::string::npos);
            }
        }
    }

    GIVEN("the consumer's full transcript — three asymmetric calls + im_end") {
        // Same shape as the actual repro: model emits the asymmetric
        // call three times back to back with <|im_end|> turn markers
        // interleaved. All three should parse.
        std::string content =
            R"(<|tool_call>{"name":"entropic.delegate","arguments":{"target":"a","task":"x"}}</tool_call>)"
            "\n"
            R"(<|tool_call>{"name":"entropic.delegate","arguments":{"target":"b","task":"y"}}</tool_call>)"
            "\n<|im_end|>\n"
            R"(<|tool_call>{"name":"entropic.delegate","arguments":{"target":"c","task":"z"}}</tool_call>)"
            "\n<|im_end|>\n";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);

            THEN("all three calls are extracted") {
                REQUIRE(result.tool_calls.size() == 3);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
                REQUIRE(result.tool_calls[1].name == "entropic.delegate");
                REQUIRE(result.tool_calls[2].name == "entropic.delegate");
            }
        }
    }

    GIVEN("the fully-symmetric special-token form <|tool_call|>...<|/tool_call|>") {
        // Defensive — if a future llama.cpp pin decodes the special
        // token to its true symmetric form, accept it too. Close tag
        // we still expect to land as </tool_call> in practice; if
        // <|/tool_call|> shows up we'll need a separate fix.
        std::string content =
            R"(<|tool_call|>{"name":"entropic.delegate","arguments":{"target":"a","task":"x"}}</tool_call>)";

        WHEN("parse_tool_calls is called") {
            auto result = adapter.parse_tool_calls(content);
            THEN("the call is extracted") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
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

// ── gh#68 (v2.3.5): chat-template marker scrub ─────────────

// Sibling of gh#65: Gemma 4 emits its chat-template markers as
// multi-token regular surface tokens. They reach the adapter via
// `content`; the scrub strips them so they never appear in
// `cleaned_content` (the assistant-visible body).
//
// v2.3.4 attempted to fix this at the detokenize layer via
// `special=false` but it was a no-op for Gemma 4 (the tokens
// aren't classified as special). v2.3.5 moves the fix to the
// adapter layer — same surface gh#65 already uses for the
// asymmetric `<|tool_call>` scrub.

SCENARIO("Gemma4 scrubs <|im_end|> from cleaned_content (gh#68)",
         "[gemma4][gh68][marker-scrub]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("a model response ending in <|im_end|> (the exact gh#68 repro)") {
        // Consumer's transcript: 6 tokens → 10 chars of literal
        // <|im_end|>. The content lands at the adapter via
        // `parse_tool_calls(content)`.
        std::string content = "I am sorry, I don't understand.<|im_end|>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("cleaned_content has NO <|im_end|> substring") {
                REQUIRE(result.cleaned_content.find("<|im_end|>")
                        == std::string::npos);
            }
            THEN("the prose preceding the marker survives unchanged") {
                REQUIRE(result.cleaned_content.find(
                    "I am sorry, I don't understand.")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 scrubs the asymmetric <|im_end> form (gh#68)",
         "[gemma4][gh68][marker-scrub]") {
    entropic::Gemma4Adapter adapter("lead", "test");

    GIVEN("the asymmetric variant (no trailing pipe — same shape as gh#65)") {
        // If the tokenizer surface drops `|>` on some emit path
        // (like gh#65 documented for `<|tool_call>`), the scrub
        // must handle the asymmetric form too.
        std::string content = "Hello there.<|im_end>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("cleaned_content has no `<|im_end` substring") {
                // Catches both `<|im_end>` and `<|im_end|>`.
                REQUIRE(result.cleaned_content.find("<|im_end")
                        == std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 scrubs <|im_start|> turn markers (gh#68)",
         "[gemma4][gh68][marker-scrub]") {
    entropic::Gemma4Adapter adapter("lead", "test");

    GIVEN("content with <|im_start|> turn-open markers (bare + role-suffixed)") {
        std::string content =
            "<|im_start|>assistantHere is my reply.<|im_end|>"
            "<|im_start|>userExtra<|im_end|>"
            "<|im_start|>Tail";  // bare form, no role suffix

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("no <|im_start| substring remains") {
                REQUIRE(result.cleaned_content.find("<|im_start")
                        == std::string::npos);
            }
            THEN("the surrounding prose survives") {
                REQUIRE(result.cleaned_content.find("Here is my reply.")
                        != std::string::npos);
                REQUIRE(result.cleaned_content.find("Tail")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 scrubs <end_of_turn> and <start_of_turn> markers (gh#68)",
         "[gemma4][gh68][marker-scrub]") {
    entropic::Gemma4Adapter adapter("lead", "test");

    GIVEN("content using the canonical Gemma 4 turn markers") {
        // Gemma 4's chat template canonically uses `<end_of_turn>` /
        // `<start_of_turn>`. The pipe-bracket variants are the
        // tokenizer-decomposed surface forms; both can leak.
        std::string content =
            "<start_of_turn>modelMy reply.<end_of_turn>"
            "<start_of_turn>userFollow-up<end_of_turn>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("neither marker family appears in cleaned_content") {
                REQUIRE(result.cleaned_content.find("<end_of_turn>")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("<start_of_turn>")
                        == std::string::npos);
            }
            THEN("prose survives") {
                REQUIRE(result.cleaned_content.find("My reply.")
                        != std::string::npos);
                REQUIRE(result.cleaned_content.find("Follow-up")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 marker scrub doesn't eat tool_call content (gh#68 "
         "interaction with gh#65)",
         "[gemma4][gh68][marker-scrub]") {
    entropic::Gemma4Adapter adapter("lead", "test");

    GIVEN("a turn that emits both a tool_call AND a trailing <|im_end|>") {
        // Realistic shape: model emits delegate then end-of-turn.
        // Tool call should still parse; cleaned_content should be
        // empty (or whitespace) — both the tool_call markup AND the
        // turn marker should be gone.
        std::string content =
            R"(<|tool_call>{"name":"entropic.delegate",)"
            R"("arguments":{"target":"x","task":"y"}}</tool_call>)"
            "<|im_end|>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("the tool call still parses (gh#65 path intact)") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
            }
            THEN("cleaned_content has neither tool_call markup nor <|im_end|>") {
                REQUIRE(result.cleaned_content.find("<|tool_call")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("</tool_call>")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("<|im_end")
                        == std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 marker scrub preserves angle-bracket content that ISN'T a "
         "template marker (gh#68 regression check)",
         "[gemma4][gh68][marker-scrub]") {
    entropic::Gemma4Adapter adapter("lead", "test");

    GIVEN("content with regex-tempting but non-template angle-bracket text") {
        // Defense: scrub regex must not over-match. Plain HTML-style
        // tags, math notation, code snippets containing angle brackets
        // must survive intact.
        std::string content =
            "Use the operator `<<` for left-shift. "
            "Set `<input type=\"text\">` in your HTML. "
            "Inequalities: `x < y && y > z`.";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("all the angle-bracket prose survives intact") {
                REQUIRE(result.cleaned_content.find("`<<`")
                        != std::string::npos);
                REQUIRE(result.cleaned_content.find("<input type=")
                        != std::string::npos);
                REQUIRE(result.cleaned_content.find("x < y && y > z")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 marker scrub handles empty + whitespace-only content "
         "(gh#68 degenerate cases)",
         "[gemma4][gh68][marker-scrub]") {
    entropic::Gemma4Adapter adapter("lead", "test");

    WHEN("content is empty") {
        auto result = adapter.parse_tool_calls("");
        THEN("cleaned_content is empty, no crash") {
            REQUIRE(result.cleaned_content == "");
            REQUIRE(result.tool_calls.empty());
        }
    }

    WHEN("content is ONLY a turn marker (the gh#68 6-token decoded path)") {
        // The actual consumer transcript: 6 tokens, all 10 chars of
        // <|im_end|>. After scrub, cleaned_content should be empty.
        auto result = adapter.parse_tool_calls("<|im_end|>");
        THEN("cleaned_content is empty (no leak)") {
            REQUIRE(result.cleaned_content == "");
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
