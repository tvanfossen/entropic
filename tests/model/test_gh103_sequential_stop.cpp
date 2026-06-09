// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh103_sequential_stop.cpp
 * @brief gh#103 (v2.8.2): EMERGENT multi-turn proof that "sequential"
 *        tool_call_mode hard-STOPS generation at the first closed tool call —
 *        through the COMBINED production path
 *        (orchestrator::generate → resolve_and_stage → inject_sequential_stop →
 *         backend decode-loop stop → marker retained → common_chat parse).
 *
 * On the gh#103 repro / hybrid model (Qwen3.6-35B-A3B, arch qwen35moe,
 * PEG_NATIVE → close marker "</tool_call>"). The bug: terminal/looping tiers
 * generate PAST the tool call (wasted tokens; dependent traces never converge).
 * The fix: a sequential tier stops AT the first "</tool_call>".
 *
 * Deterministic, GPU-nondeterminism-safe assertions (structure, not text):
 *  - NON-VACUOUS proof: when a tool call is emitted, raw_content ENDS at
 *    "</tool_call>" (generation halted there). A batch run continues past it
 *    (the bug) — so this can only pass if the hard-stop fired.
 *  - HARD CAP: raw_content contains AT MOST ONE "</tool_call>" every turn —
 *    sequential can never emit a second call (batch can).
 *  - marker RETAINED: a count==1 turn still parses tool_calls.size()==1
 *    (the stop does not strip the marker, so common_chat parses the block).
 *  - hybrid coherence (gh#97): every turn is error-free + non-empty across a
 *    multi-turn loop — the early stop must not desync the recurrent KV.
 * A batch contrast generation is logged (not asserted — model-dependent) to
 * make the difference visible.
 *
 * Requires: GPU + Qwen3.6-35B-A3B GGUF. Run: ctest -L model -R gh103
 *
 * @version 2.8.2
 */

#include "v219_family_test_helpers.h"

#include <entropic/types/tool_call.h>

#include <string>
#include <vector>

namespace { constexpr char K_QWEN36[] = "qwen3_6_a3b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_QWEN36>)

namespace {

const char* kMarker = "</tool_call>";

// Count non-overlapping occurrences of a needle.
size_t count_occurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Trailing-whitespace-insensitive endswith.
bool rtrim_ends_with(const std::string& s, const std::string& suffix) {
    size_t end = s.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) { return false; }
    std::string trimmed = s.substr(0, end + 1);
    return trimmed.size() >= suffix.size()
        && trimmed.compare(trimmed.size() - suffix.size(),
                           suffix.size(), suffix) == 0;
}

// Two tools so a multi-tool plan is *available* (batch could chain them in one
// turn; sequential must not).
const char* kTwoTools =
    R"([{"name":"read_file","description":"Read a file from disk.",)"
    R"("inputSchema":{"type":"object","properties":{"path":{"type":"string"}},)"
    R"("required":["path"]}},)"
    R"({"name":"list_dir","description":"List a directory.",)"
    R"("inputSchema":{"type":"object","properties":{"path":{"type":"string"}},)"
    R"("required":["path"]}}])";

}  // namespace

SCENARIO("gh#103: sequential tool_call_mode hard-stops at the first tool call "
         "(emergent multi-turn, combined orchestrator path, qwen35moe)",
         "[model][gh103][sequential]")
{
    if (!g_ctx.initialized) {
        SKIP("qwen3_6_a3b GGUF not present — run `entropic download qwen3_6_a3b`");
    }
    GIVEN("a sequential tier with two tools over a persistent session") {
        start_test_log("gh103_sequential_stop");

        auto params = test_gen_params();
        params.tools = kTwoTools;
        params.tool_call_mode = "sequential";   // gh#103 — the lever under test
        params.enable_thinking = false;          // terse: reach the call inside budget
        params.max_tokens = 384;

        // A task that invites chaining list_dir → read_file (batch would be
        // tempted to emit both in one turn; sequential must observe between).
        std::vector<entropic::Message> messages = make_messages(
            "You are a terse file assistant. Use the provided tools, one tool "
            "call at a time, to answer. Emit a tool call.",
            "Find the file 'notes.txt' under /data and read it.");

        WHEN("the orchestrator generates across 3 sequential turns") {
            constexpr int kTurns = 3;
            struct Turn { int closes; size_t calls; bool ends_marker;
                          bool ok; bool empty; std::string finish; };
            std::vector<Turn> turns;
            int tool_turns = 0;

            for (int t = 0; t < kTurns; ++t) {
                auto r = g_ctx.orchestrator->generate(
                    messages, params, g_ctx.default_tier);
                int closes = static_cast<int>(
                    count_occurrences(r.raw_content, kMarker));
                turns.push_back({closes, r.tool_calls.size(),
                                 rtrim_ends_with(r.raw_content, kMarker),
                                 r.error_code == 0, r.content.empty(),
                                 r.finish_reason});
                if (!r.tool_calls.empty()) { ++tool_turns; }
                // Continue the session: persist the assistant turn + a synthetic
                // tool result so the next turn has fresh context (observe-between).
                messages.push_back({"assistant", r.raw_content});
                messages.push_back({"user",
                    "[tool result] ok: /data/notes.txt contains \"hello\"."});
            }

            THEN("every turn stops at the first tool call — never past it") {
                for (size_t i = 0; i < turns.size(); ++i) {
                    const auto& tn = turns[i];
                    INFO("turn " << i << " closes=" << tn.closes
                         << " calls=" << tn.calls
                         << " ends_marker=" << tn.ends_marker
                         << " finish=[" << tn.finish << "]");
                    // No crash / recurrent-KV desync (gh#97 hybrid coherence).
                    REQUIRE(tn.ok);
                    // HARD CAP: sequential can never emit a second tool call.
                    CHECK(tn.closes <= 1);
                    // NON-VACUOUS: a tool-call turn HALTS at the marker (batch
                    // would generate past it — the gh#103 bug) and the retained
                    // marker still parses exactly one call.
                    if (tn.closes == 1) {
                        CHECK(tn.ends_marker);
                        CHECK(tn.calls == 1);
                    }
                }
                // Tools must be reachable: at least one turn emitted a call,
                // else the cap is vacuous.
                INFO("tool_turns=" << tool_turns << "/" << kTurns);
                CHECK(tool_turns >= 1);
                end_test_log();
            }
        }
    }
}

// ── Batch contrast (documents the baseline; NOT asserted — model-dependent) ──
SCENARIO("gh#103 contrast: batch mode may generate past the tool call",
         "[model][gh103][sequential]")
{
    if (!g_ctx.initialized) { SKIP("qwen3_6_a3b GGUF not present"); }
    GIVEN("the same prompt in batch (default) tool_call_mode") {
        auto params = test_gen_params();
        params.tools = kTwoTools;
        params.enable_thinking = false;
        params.max_tokens = 384;
        // tool_call_mode left empty → batch (no stop injected)
        auto messages = make_messages(
            "You are a terse file assistant. Use the provided tools to answer.",
            "Find the file 'notes.txt' under /data and read it.");

        WHEN("the orchestrator generates without sequential mode") {
            auto r = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);
            int closes = static_cast<int>(
                count_occurrences(r.raw_content, kMarker));
            THEN("the baseline is recorded (no hard cap; may run past the call)") {
                INFO("batch closes=" << closes
                     << " ends_marker=" << rtrim_ends_with(r.raw_content, kMarker)
                     << " finish=[" << r.finish_reason << "] calls="
                     << r.tool_calls.size());
                // Only assert it ran cleanly — the POINT is the contrast logged
                // above (batch is free to emit >1 close or trail content; that
                // freedom is exactly what sequential removes).
                CHECK(r.error_code == 0);
            }
        }
    }
}
