// SPDX-License-Identifier: Apache-2.0
/**
 * @file empty_content_diagnosis_test.cpp
 * @brief gh#137: an empty turn must be explained by its ACTUAL cause.
 *
 * @par What this pins
 * gh#137 reported `Generate complete (batch): finish=stop, 154 chars` delivering
 * a turn with 0 chars. The engine's response was to tell the operator to raise
 * max_tokens — advice that is only correct when the generation was TRUNCATED.
 * With `finish_reason == "stop"` the model ended the turn itself while still
 * inside a reasoning block; more budget changes nothing, and the message sent
 * the reporter looking in the wrong place.
 *
 * The underlying gh#137 defect (why the model reasons from token 0 on a tier
 * with `enable_thinking: false`) did NOT reproduce on the hardware available —
 * two GPU runs on gemma-4 E2B QAT emitted no channel markers at all and
 * delivered 100% of their content. What IS provable without a reproduction is
 * that the diagnostic misattributes the cause, so that is what this pins.
 *
 * gh#159 (v2.13.0) added the fourth argument, `has_tool_calls`. Every case
 * below passes it explicitly — the parameter has no default precisely so a
 * caller cannot omit what it knows.
 *
 * @version 2.13.0
 */

#include "empty_content_diagnosis.h"

#include <catch2/catch_test_macros.hpp>

using entropic::diagnose_empty_content;
using entropic::EmptyContentCause;
using entropic::explain_empty_content;

SCENARIO("gh#137 an empty turn is attributed to its real cause",
         "[inference][gh137][diagnostics][cpu]")
{
    GIVEN("content survived the strip") {
        THEN("there is nothing to explain, whatever the finish reason") {
            CHECK(diagnose_empty_content(false, true, "stop", false)
                  == EmptyContentCause::not_empty);
            CHECK(diagnose_empty_content(false, true, "length", false)
                  == EmptyContentCause::not_empty);
            CHECK(explain_empty_content(EmptyContentCause::not_empty).empty());
        }
    }

    GIVEN("the model produced nothing at all") {
        THEN("empty content is not a strip problem and is not reported as one") {
            CHECK(diagnose_empty_content(true, false, "stop", false)
                  == EmptyContentCause::not_empty);
        }
    }

    GIVEN("tokens were produced, content is empty, finish_reason is length") {
        auto cause = diagnose_empty_content(true, true, "length", false);

        THEN("it is a budget problem") {
            CHECK(cause == EmptyContentCause::budget_truncated);
        }
        THEN("the advice names max_tokens, because that IS the fix") {
            auto msg = explain_empty_content(cause);
            CHECK(msg.find("max_tokens") != std::string::npos);
        }
    }

    GIVEN("the gh#137 shape — tokens produced, content empty, finish=stop") {
        auto cause = diagnose_empty_content(true, true, "stop", false);

        THEN("it is the model stopping, not a budget ceiling") {
            CHECK(cause == EmptyContentCause::model_stopped);
        }

        THEN("the advice must NOT tell the operator to raise max_tokens") {
            // The regression that matters. The pre-2.11.0 message said
            // "Raise max_tokens." on exactly this input, which cannot help a
            // turn the model chose to end, and is what sent gh#137's reporter
            // down the wrong path.
            auto msg = explain_empty_content(cause);
            CHECK(msg.find("raising max_tokens will not help")
                  != std::string::npos);
            CHECK(msg.find("finish_reason is 'stop'") != std::string::npos);
        }
    }

    GIVEN("an unattributable finish reason") {
        auto cause = diagnose_empty_content(true, true, "error", false);

        THEN("the engine admits it cannot explain the turn") {
            CHECK(cause == EmptyContentCause::unknown);
            auto msg = explain_empty_content(cause);
            CHECK_FALSE(msg.empty());
            // Must not guess at a cause it does not have evidence for.
            CHECK(msg.find("max_tokens") == std::string::npos);
        }
    }
}

// ── gh#159 (v2.13.0): a bare tool-call turn is not an empty-content fault ──
//
// Tier `clew`, `enable_thinking: false`, `tool_call_mode: sequential`. The
// model emitted 21 tokens, all of them one tool call. `apply_adapter_parse`
// moved the call out of `content` — that is what it is for — leaving content
// empty, raw_content non-empty and finish_reason "stop": byte-for-byte the
// gh#137 shape. The engine then told the operator the model "ended the turn
// while still inside an unterminated reasoning block" and that "the prompt or
// tier configuration is not converging on an answer", on a tier with thinking
// switched off. The reporter spent a debugging pass on `enable_thinking`
// before noticing it was already false. It fired on all four clew tool-call
// turns and every researcher turn in the session.
//
// The diagnosis was right about its inputs and wrong about the world: it has
// no tool-call input, so it could not see the one fact that distinguishes a
// well-behaved sequential turn from a stalled one.

SCENARIO("gh#159 a turn whose whole output was a tool call is not a fault",
         "[inference][gh159][diagnostics][cpu]")
{
    GIVEN("the reported turn: content empty, raw non-empty, finish=stop, "
          "one parsed tool call") {
        auto cause = diagnose_empty_content(
            /*content_empty=*/true, /*produced_tokens=*/true,
            /*finish_reason=*/"stop", /*has_tool_calls=*/true);

        THEN("it is named as a tool-call turn, not a stalled one") {
            CHECK(cause == EmptyContentCause::tool_call_only);
        }

        THEN("the explanation says nothing about reasoning blocks") {
            // The sentence that did the damage. A consumer tuning a tier
            // ACTS on "not converging on an answer" — it reads as a
            // diagnosis, and it was one, of a problem that did not exist.
            auto msg = explain_empty_content(cause);
            CHECK(msg.find("reasoning") == std::string::npos);
            CHECK(msg.find("not converging") == std::string::npos);
            CHECK(msg.find("max_tokens") == std::string::npos);
        }

        THEN("it still explains itself, so the empty content is accounted for") {
            CHECK_FALSE(explain_empty_content(cause).empty());
        }
    }

    GIVEN("the same shape with NO tool call — gh#137's real case") {
        auto cause = diagnose_empty_content(true, true, "stop", false);

        THEN("the reasoning-block diagnosis is intact") {
            // The control. gh#137 is a real failure and its message must
            // survive the fix that stops it firing on healthy turns.
            CHECK(cause == EmptyContentCause::model_stopped);
            auto msg = explain_empty_content(cause);
            CHECK(msg.find("unterminated reasoning block")
                  != std::string::npos);
            CHECK(msg.find("not converging") != std::string::npos);
        }
    }

    GIVEN("a tool call on a turn that ALSO delivered prose") {
        THEN("there is nothing to explain — content survived") {
            CHECK(diagnose_empty_content(false, true, "stop", true)
                  == EmptyContentCause::not_empty);
        }
    }

    GIVEN("a tool-call turn that also hit the token budget") {
        // A call was parsed, so the turn is not a stall — but `length`
        // means the decode was cut off, which is worth keeping distinct
        // from a clean sequential stop. The tool call wins: a parsed call
        // is evidence the turn did its job, and gh#134's
        // budget-starved-REQUIRED warning owns the truncation case.
        THEN("it is still a tool-call turn, not a budget fault") {
            CHECK(diagnose_empty_content(true, true, "length", true)
                  == EmptyContentCause::tool_call_only);
        }
    }
}
