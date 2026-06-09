// SPDX-License-Identifier: Apache-2.0
/**
 * @file tool_call_markers_test.cpp
 * @brief gh#103 (v2.8.2): CPU pin for the family-aware tool-call CLOSE marker
 *        map (tool_call_markers.h), used by the "sequential" tool_call_mode to
 *        hard-stop generation at the first closed tool call.
 *
 * The map is vendor-coupled to the common_chat PEG `section_end` defaults
 * (extern/llama.cpp/common/chat-peg-parser.cpp:442-443). This test pins the
 * contract — it fails RED if a marker is changed/dropped, flagging a needed
 * re-check on a llama.cpp pin bump. The gh#103 MODEL test then validates the
 * marker actually fires against real model output (this can't, being model-free).
 *
 * @version 2.8.2
 */

#include <catch2/catch_test_macros.hpp>

#include "tool_call_markers.h"

using entropic::append_sequential_stop;
using entropic::close_marker_for_format;

SCENARIO("gh#103: tool-call close markers map per resolved common_chat format",
         "[gh103][inference][cpu][tool_call_mode]")
{
    GIVEN("the PEG_NATIVE / PEG_SIMPLE formats (qwen3 / hermes / nemotron)") {
        THEN("the per-call close marker is </tool_call> (PEG section_end)") {
            CHECK(close_marker_for_format(COMMON_CHAT_FORMAT_PEG_NATIVE)
                  == "</tool_call>");
            CHECK(close_marker_for_format(COMMON_CHAT_FORMAT_PEG_SIMPLE)
                  == "</tool_call>");
        }
    }

    GIVEN("the gemma4 PEG format") {
        THEN("the per-call close marker is <tool_call|> (gh#103 consumer "
             "transcript + chat.cpp:1178), distinct from the open <|tool_call>") {
            CHECK(close_marker_for_format(COMMON_CHAT_FORMAT_PEG_GEMMA4)
                  == "<tool_call|>");
        }
    }

    GIVEN("content-only / unknown formats") {
        THEN("the marker is empty → no stop injected (batch-safe fallback)") {
            CHECK(close_marker_for_format(COMMON_CHAT_FORMAT_CONTENT_ONLY)
                  .empty());
        }
    }
}

// ── Engagement guard: append_sequential_stop must fire ONLY for sequential ──
// This is the instrumentation assertion (TDD-red-first): it FAILS if the
// gh#103 hard-stop lever does not engage — independent of any model's natural
// verbosity, which a GPU model test alone cannot guarantee (a terse model may
// stop at the marker on its own, passing vacuously).
SCENARIO("gh#103: append_sequential_stop engages only in sequential mode",
         "[gh103][inference][cpu][tool_call_mode]")
{
    GIVEN("a sequential-mode params + a non-empty close marker") {
        entropic::GenerationParams p;
        p.tool_call_mode = "sequential";
        THEN("the marker is appended to stop (the lever engages)") {
            append_sequential_stop(p, "</tool_call>");
            REQUIRE(p.stop.size() == 1);
            CHECK(p.stop[0] == "</tool_call>");
        }
        AND_THEN("a second call with the same marker does not duplicate it") {
            append_sequential_stop(p, "</tool_call>");
            append_sequential_stop(p, "</tool_call>");
            CHECK(p.stop.size() == 1);
        }
    }

    GIVEN("batch (default/empty) mode") {
        entropic::GenerationParams p;  // tool_call_mode == "" by default
        THEN("no stop is injected even with a valid marker") {
            append_sequential_stop(p, "</tool_call>");
            CHECK(p.stop.empty());
        }
    }

    GIVEN("sequential mode but an empty marker (e.g. gemma/content-only)") {
        entropic::GenerationParams p;
        p.tool_call_mode = "sequential";
        THEN("no stop is injected (batch-safe fallback)") {
            append_sequential_stop(p, "");
            CHECK(p.stop.empty());
        }
    }
}
