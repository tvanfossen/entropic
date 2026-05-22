// SPDX-License-Identifier: Apache-2.0
/**
 * @file adapter_acceptance_test.cpp
 * @brief Per-adapter tool-call acceptance gate (v2.3.8, gh#71).
 *
 * @par Why this file exists
 * gh#69 (gemma4) and gh#70 (nemotron3) both shipped an adapter whose
 * tool-call format did not match what the model actually emits — and
 * both passed the existing suite. gh#71 traced that to four structural
 * gaps in the old adapter-validation methodology:
 *
 *   1. **Tautological unit tests** — fed the adapter the format it
 *      *assumes* (e.g. synthetic `<function=...>` XML for nemotron3),
 *      so a wrong assumption about the model's output was unfalsifiable.
 *   2. **Rigged model-test prompts** — handed the model the exact tag
 *      template in the system prompt, testing template-copying rather
 *      than emission under the real `format_tools` agent loop.
 *   3. **Loose assertions** — `REQUIRE(tool_calls.size() >= 1)`, i.e.
 *      "extracted *something*".
 *   4. **Skip-gated** — `SKIP(...)` when the GGUF/GPU is absent, so the
 *      real-emission tests never ran in CI; "N/N passed" hid the skips.
 *
 * @par What this gate does differently
 *   - **Verbatim consumer-captured emits.** Every fixture below is the
 *     raw string a real model produced, pasted as a literal — NOT
 *     adapter-shaped synthetic content. A fixture that matches the
 *     adapter's *assumption* cannot catch a wrong assumption.
 *   - **Strict assertions.** Exact call count, exact tool name, expected
 *     argument keys/values, AND zero markup leakage into
 *     `cleaned_content`.
 *   - **No SKIP, no model, no GPU.** Operates on captured emits at the
 *     CPU layer, so it runs on every commit on every CI machine.
 *
 * @par Dispositive property
 * Each `[gh69]` / `[gh70]` scenario FAILS on the pre-v2.3.8 adapter
 * source and PASSES post-fix. That is the proof the gate is at the right
 * layer: if the fix were wrong and the bug persisted, these would fail.
 * The `[positive-anchor]` scenarios confirm the gate does not
 * false-positive on adapters whose format was already correct.
 *
 * NOTE: tests/ is excluded from knots + doxygen-guard; this file is held
 * to the same readability bar regardless.
 *
 * @version 2.3.8
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "adapters/gemma4_adapter.h"
#include "adapters/nemotron3_adapter.h"
#include "adapters/qwen35_adapter.h"
#include "adapters/qwen36_adapter.h"

// ── gh#69: Gemma 4 <|im_start|>tool_call channel form ──────

SCENARIO("Gemma4 parses the gh#69 <|im_start|>tool_call channel emit",
         "[adapter-acceptance][gemma4][gh69]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("the verbatim e4b channel emit from the gh#69 report") {
        // Opening header is `<|im_start|>tool_call` (NOT `<tool_call>`);
        // close is the plain `</tool_call>`; a turn `<|im_end|>` trails.
        std::string content =
            "<|im_start|>tool_call\n"
            R"({"name": "entropic.delegate", "arguments": )"
            R"({"target": "researcher", "task": )"
            R"("Find the findme command in the codebase."}})"
            "</tool_call><|im_end|>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("exactly one delegate call is extracted") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
            }
            THEN("the target argument round-trips") {
                REQUIRE(result.tool_calls.size() == 1);
                auto it = result.tool_calls[0].arguments.find("target");
                REQUIRE(it != result.tool_calls[0].arguments.end());
                REQUIRE(it->second.find("researcher") != std::string::npos);
            }
            THEN("no channel markup leaks into cleaned_content") {
                REQUIRE(result.cleaned_content.find("<|im_start|>")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("tool_call")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("</tool_call>")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("<|im_end")
                        == std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 parses a multi-call gh#69 channel sequence",
         "[adapter-acceptance][gemma4][gh69]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("two back-to-back channel calls with interleaved <|im_end|>") {
        std::string content =
            "<|im_start|>tool_call\n"
            R"({"name": "entropic.delegate", "arguments": )"
            R"({"target": "researcher", "task": "x"}})"
            "</tool_call><|im_end|>\n"
            "<|im_start|>tool_call\n"
            R"({"name": "entropic.followup", "arguments": )"
            R"({"query": "findme"}})"
            "</tool_call><|im_end|>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("both calls are extracted in order, by exact name") {
                REQUIRE(result.tool_calls.size() == 2);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
                REQUIRE(result.tool_calls[1].name == "entropic.followup");
            }
            THEN("nothing leaks into cleaned_content") {
                REQUIRE(result.cleaned_content.find("tool_call")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("<|im_")
                        == std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 channel call preserves trailing assistant prose (gh#69)",
         "[adapter-acceptance][gemma4][gh69]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("a channel call followed by visible assistant text") {
        std::string content =
            "<|im_start|>tool_call\n"
            R"({"name": "entropic.followup", "arguments": {"query": "x"}})"
            "</tool_call><|im_end|>\n"
            "I have queued the lookup.";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("the call parses and the prose survives clean") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.followup");
                REQUIRE(result.cleaned_content.find("I have queued the lookup.")
                        != std::string::npos);
                REQUIRE(result.cleaned_content.find("tool_call")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("<|im_")
                        == std::string::npos);
            }
        }
    }
}

// ── Prior gemma4 regressions (gh#65 + gh#68) ──────────────

SCENARIO("Gemma4 parses the gh#65 asymmetric <|tool_call> form",
         "[adapter-acceptance][gemma4][gh65]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("the verbatim gh#65 v2.3.3 consumer-report emit") {
        // Asymmetric open `<|tool_call>` (pipe-prefixed, no trailing
        // `|>`), plain `</tool_call>` close — the v2.3.3 fix added
        // this variant; the gh#69 fix must not have regressed it.
        std::string content =
            R"(<|tool_call>{"name": "entropic.delegate", "arguments": )"
            R"({"target": "registrar", "task": "List all configured )"
            R"(subjects for this family."}}</tool_call>)";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("exactly one delegate call is extracted") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
                REQUIRE(result.tool_calls[0].arguments.count("target") == 1);
            }
            THEN("no tool-call markup leaks into cleaned_content") {
                REQUIRE(result.cleaned_content.find("<|tool_call")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("</tool_call>")
                        == std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 scrubs the gh#68 <|im_end|> turn-marker leak",
         "[adapter-acceptance][gemma4][gh68]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("the verbatim gh#68 consumer-report emit") {
        // 10-char literal `<|im_end|>` leaked into the assistant body
        // before the v2.3.5 template-marker scrub; the gh#69 channel
        // parser must not have re-exposed that text.
        std::string content =
            "I am sorry, I don't understand what 'class list' means."
            "<|im_end|>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("the turn marker is stripped") {
                REQUIRE(result.cleaned_content.find("<|im_end")
                        == std::string::npos);
            }
            THEN("the assistant prose survives intact") {
                REQUIRE(result.cleaned_content.find("I am sorry")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 format_system_prompt emits a non-empty tools section",
         "[adapter-acceptance][gemma4]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("a single tool definition") {
        std::vector<std::string> tools = {
            R"({"name": "entropic.delegate", "description": "Delegate a task",)"
            R"( "inputSchema": {"type": "object"}})"};

        WHEN("format_system_prompt assembles the prompt") {
            std::string prompt = adapter.format_system_prompt("base", tools);

            THEN("the tool name reaches the model") {
                REQUIRE(prompt.find("entropic.delegate") != std::string::npos);
            }
            THEN("the assembled prompt is not empty") {
                REQUIRE_FALSE(prompt.empty());
            }
        }
    }
}

// ── gh#70: Nemotron 3 DSML invoke form ─────────────────────

SCENARIO("Nemotron3 parses the gh#70 BF16 DSML followup emit",
         "[adapter-acceptance][nemotron3][gh70]") {
    entropic::Nemotron3Adapter adapter("lead", "test identity");

    GIVEN("the verbatim BF16 (full-weight, coherent) DSML invoke") {
        // Fullwidth-pipe `｜` (U+FF5C); self-closing typed parameter.
        std::string content =
            "<｜DSML｜function_calls>\n"
            "<｜DSML｜invoke name=\"entropic.followup\">\n"
            "<｜DSML｜parameter name=\"query\" string=\"findme command\"/>\n"
            "</｜DSML｜invoke>\n"
            "</｜DSML｜function_calls>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("exactly one followup call is extracted") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.followup");
            }
            THEN("the query argument is captured verbatim") {
                REQUIRE(result.tool_calls.size() == 1);
                auto it = result.tool_calls[0].arguments.find("query");
                REQUIRE(it != result.tool_calls[0].arguments.end());
                REQUIRE(it->second == "findme command");
            }
            THEN("no DSML markup leaks into cleaned_content") {
                REQUIRE(result.cleaned_content.find("DSML")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("\xEF\xBD\x9C")  // ｜
                        == std::string::npos);
            }
        }
    }
}

SCENARIO("Nemotron3 parses a multi-parameter gh#70 DSML delegate",
         "[adapter-acceptance][nemotron3][gh70]") {
    entropic::Nemotron3Adapter adapter("lead", "test identity");

    GIVEN("a DSML delegate invoke with target + task parameters") {
        std::string content =
            "<｜DSML｜function_calls>\n"
            "<｜DSML｜invoke name=\"entropic.delegate\">\n"
            "<｜DSML｜parameter name=\"target\" string=\"researcher\"/>\n"
            "<｜DSML｜parameter name=\"task\" string=\"Explore the codebase "
            "for the findme command.\"/>\n"
            "</｜DSML｜invoke>\n"
            "</｜DSML｜function_calls>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("one delegate call with both parameters is extracted") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
                const auto& args = result.tool_calls[0].arguments;
                REQUIRE(args.at("target") == "researcher");
                REQUIRE(args.at("task").find("Explore the codebase")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Nemotron3 parses DSML amid Q8 begin-of-sentence spam (gh#70)",
         "[adapter-acceptance][nemotron3][gh70]") {
    entropic::Nemotron3Adapter adapter("lead", "test identity");

    GIVEN("a DSML invoke wrapped in <｜begin▁of▁sentence｜> BOS spam") {
        // The Q8_0 quant interleaves BOS tokens around the call; the
        // call must still parse and the BOS spam must be scrubbed from
        // cleaned_content. `▁` is U+2581.
        std::string content =
            "<｜begin▁of▁sentence｜><｜begin▁of▁sentence｜>"
            "<｜DSML｜function_calls>\n"
            "<｜DSML｜invoke name=\"entropic.followup\">\n"
            "<｜DSML｜parameter name=\"query\" string=\"findme command\"/>\n"
            "</｜DSML｜invoke>\n"
            "</｜DSML｜function_calls>"
            "<｜begin▁of▁sentence｜>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("the call still parses despite the spam") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.followup");
                REQUIRE(result.tool_calls[0].arguments.at("query")
                        == "findme command");
            }
            THEN("the BOS spam and DSML markup are scrubbed") {
                REQUIRE(result.cleaned_content.find("begin")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("DSML")
                        == std::string::npos);
                REQUIRE(result.cleaned_content.find("\xEF\xBD\x9C")  // ｜
                        == std::string::npos);
            }
        }
    }
}

SCENARIO("Nemotron3 format_tools teaches the DSML invoke format (gh#70)",
         "[adapter-acceptance][nemotron3][gh70]") {
    entropic::Nemotron3Adapter adapter("lead", "test identity");

    GIVEN("a tool definition injected via the public system-prompt path") {
        // format_tools is protected; reach it through the public
        // format_system_prompt, which is what the engine actually calls.
        std::vector<std::string> tools = {
            R"({"name": "entropic.delegate", "description": "Delegate a task",)"
            R"( "inputSchema": {"type": "object"}})"};

        WHEN("format_system_prompt assembles the tool section") {
            std::string prompt = adapter.format_system_prompt("base", tools);

            THEN("it teaches the DSML invoke form, not qwen XML") {
                REQUIRE(prompt.find("<｜DSML｜invoke") != std::string::npos);
                REQUIRE(prompt.find("<｜DSML｜parameter")
                        != std::string::npos);
                REQUIRE(prompt.find("<function=example_function>")
                        == std::string::npos);
            }
            THEN("the tool name still reaches the model in the <tools> block") {
                REQUIRE(prompt.find("entropic.delegate") != std::string::npos);
            }
        }
    }
}

SCENARIO("Nemotron3 still parses the qwen3_coder XML backstop",
         "[adapter-acceptance][nemotron3][backstop]") {
    entropic::Nemotron3Adapter adapter("lead", "test identity");

    GIVEN("a rigged-prompt qwen XML emit (pre-v2.3.8 default path)") {
        // The DSML primary path (gh#70) added a new parser; the qwen XML
        // path stays parseable so a downstream consumer who rigs the
        // prompt to force XML still gets a call.
        std::string content =
            "<function=entropic.delegate>\n"
            "<parameter=target>researcher</parameter>\n"
            "<parameter=task>find files</parameter>\n"
            "</function>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("the qwen-XML call still parses with both params") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
                REQUIRE(result.tool_calls[0].arguments.at("target")
                        == "researcher");
            }
        }
    }
}

// ── gh#71-phase-2: weak-model emit shapes (Gemma 4 E2B) ────

SCENARIO("Gemma4 accepts the tool_name key alias (E2B verbatim emit)",
         "[adapter-acceptance][gemma4][gh71][tool-name-alias]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("the verbatim E2B emit using tool_name instead of name") {
        // Captured from gemma-4-E2B-it-Q8_0 under the production prompt:
        // a well-formed call in everything but the key spelling.
        std::string content =
            R"(<tool_call>{"tool_name": "entropic.followup", )"
            R"("arguments": {"query": "findme"}}</tool_call>)";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("the tool_name alias resolves to a parsed call") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.followup");
                REQUIRE(result.tool_calls[0].arguments.at("query")
                        == "\"findme\"");
                REQUIRE(result.cleaned_content.find("tool_call")
                        == std::string::npos);
            }
        }
    }
}

SCENARIO("Gemma4 correctly rejects a nameless tool call (E2B limitation)",
         "[adapter-acceptance][gemma4][gh71]") {
    entropic::Gemma4Adapter adapter("lead", "test identity");

    GIVEN("a verbatim E2B emit with arguments but NO tool name") {
        // gemma-4-E2B-it omits the function name entirely on some
        // prompts. There is no honest way to recover which tool this is
        // (guessing from arg shape is a fragile heuristic), so the
        // adapter MUST return zero calls — this pins that deliberate
        // behavior so a future "recovery" hack doesn't sneak in.
        std::string content = R"(<tool_call>{"path": "/etc/hostname"}</tool_call>)";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("no tool call is fabricated from a nameless payload") {
                REQUIRE(result.tool_calls.empty());
            }
        }
    }
}

// ── Positive anchors: already-correct adapters ─────────────

SCENARIO("Qwen36 parses its native qwen3_coder XML emit",
         "[adapter-acceptance][qwen36][positive-anchor]") {
    entropic::Qwen36Adapter adapter("lead", "test identity");

    GIVEN("a native <function=...><parameter=...> emit") {
        std::string content =
            "<function=entropic.delegate>\n"
            "<parameter=target>researcher</parameter>\n"
            "<parameter=task>Explore the codebase.</parameter>\n"
            "</function>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("the call parses with both params, no markup leak") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.delegate");
                REQUIRE(result.tool_calls[0].arguments.at("target")
                        == "researcher");
                REQUIRE(result.cleaned_content.find("<function=")
                        == std::string::npos);
            }
        }
    }
}

SCENARIO("Qwen35 parses its native qwen3_coder XML emit",
         "[adapter-acceptance][qwen35][positive-anchor]") {
    entropic::Qwen35Adapter adapter("lead", "test identity");

    GIVEN("a native <function=...><parameter=...> emit") {
        std::string content =
            "<function=entropic.followup>\n"
            "<parameter=query>findme command</parameter>\n"
            "</function>";

        WHEN("parse_tool_calls runs") {
            auto result = adapter.parse_tool_calls(content);

            THEN("the call parses with its param, no markup leak") {
                REQUIRE(result.tool_calls.size() == 1);
                REQUIRE(result.tool_calls[0].name == "entropic.followup");
                REQUIRE(result.tool_calls[0].arguments.at("query")
                        == "findme command");
                REQUIRE(result.cleaned_content.find("<function=")
                        == std::string::npos);
            }
        }
    }
}
