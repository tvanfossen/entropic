// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh103_gemma_sequential.cpp
 * @brief gh#103 (v2.8.2): the gemma4 severe-case proof. A real operator session
 *        (gh#103 comment) showed gemma4 emit a complete tool call, generate PAST
 *        it, and the call register as ZERO tool calls — the terminal directive
 *        dropped entirely, not just tokens wasted. That report raised two
 *        hypotheses: (1) the runaway past the call defeats extraction → a
 *        generation hard-stop fixes it; (2) a distinct parse bug → it does not.
 *
 * This test resolves #1 vs #2 on gemma4_e4b (PEG_GEMMA4, close marker
 * `<tool_call|>`): with `tool_call_mode = "sequential"`, the orchestrator
 * injects the gemma close marker, generation stops at the first `<tool_call|>`,
 * and — the decisive assertion — the tool call **extracts** (`tool_calls`
 * non-empty). If it extracts, hypothesis #1 is confirmed and the fix lands for
 * gemma; if it does NOT extract from a clean single-call stream, that is
 * hypothesis #2 (a separate parse bug) and gh#103's hard-stop is insufficient
 * for gemma — escalate.
 *
 * Deterministic, GPU-nondeterminism-safe (structure, not text):
 *  - tool_calls.size() >= 1 in sequential mode (EXTRACTS — resolves #1/#2).
 *  - raw_content ends AT `<tool_call|>` (generation halted there, did not run
 *    past it — the consumer's exact failure shape).
 *  - at most one `<tool_call|>` (sequential can never emit a second call).
 *
 * Requires: GPU + gemma4_e4b GGUF. Run: ctest -L model -R gh103
 *
 * @version 2.8.2
 */

#include "v219_family_test_helpers.h"

#include <entropic/types/tool_call.h>

#include <string>
#include <vector>

namespace { constexpr char K_GEMMA4_E4B[] = "gemma4_e4b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_GEMMA4_E4B>)

namespace {

const char* kGemmaClose = "<tool_call|>";

size_t count_occurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

bool rtrim_ends_with(const std::string& s, const std::string& suffix) {
    size_t end = s.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) { return false; }
    std::string t = s.substr(0, end + 1);
    return t.size() >= suffix.size()
        && t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0;
}

const char* kReadFileTool =
    R"([{"name":"read_file","description":"Read a file from disk.",)"
    R"("inputSchema":{"type":"object","properties":{"path":{"type":"string"}},)"
    R"("required":["path"]}}])";

}  // namespace

SCENARIO("gh#103 gemma severe case: sequential mode stops at <tool_call|> AND "
         "the tool call extracts (resolves runaway-defeats-extraction vs parse "
         "bug) — combined orchestrator path on gemma4_e4b",
         "[model][gh103][gemma]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e4b GGUF not present — run `entropic download gemma4_e4b`");
    }
    GIVEN("gemma4_e4b, read_file staged, a tool-demanding prompt") {
        start_test_log("gh103_gemma_sequential");

        auto seq = test_gen_params();
        seq.tools = kReadFileTool;
        seq.tool_call_mode = "sequential";   // gh#103 — injects <tool_call|> stop
        seq.enable_thinking = false;
        seq.max_tokens = 256;
        auto messages = make_messages(
            "You are a terse file assistant. Use the read_file tool to answer.",
            "Read the file /etc/hostname and report its contents. "
            "Call the read_file tool.");

        WHEN("the orchestrator generates in sequential mode") {
            auto r = g_ctx.orchestrator->generate(
                messages, seq, g_ctx.default_tier);
            int closes = static_cast<int>(
                count_occurrences(r.raw_content, kGemmaClose));

            // Batch contrast (INFO only; the consumer's symptom is here — batch
            // may run past the call and extract zero). Model-dependent → logged.
            auto batch = test_gen_params();
            batch.tools = kReadFileTool;
            batch.enable_thinking = false;
            batch.max_tokens = 256;
            auto b = g_ctx.orchestrator->generate(
                messages, batch, g_ctx.default_tier);
            int b_closes = static_cast<int>(
                count_occurrences(b.raw_content, kGemmaClose));

            THEN("sequential extracts the call and halts AT <tool_call|>") {
                INFO("SEQ closes=" << closes
                     << " calls=" << r.tool_calls.size()
                     << " ends_marker=" << rtrim_ends_with(r.raw_content, kGemmaClose)
                     << " finish=[" << r.finish_reason << "]"
                     << "\n raw=[" << r.raw_content << "]"
                     << "\nBATCH closes=" << b_closes
                     << " calls=" << b.tool_calls.size()
                     << " ends_marker=" << rtrim_ends_with(b.raw_content, kGemmaClose));
                REQUIRE(r.error_code == 0);
                // Hard cap: sequential never emits a second call.
                CHECK(closes <= 1);
                // DECISIVE (#1 vs #2): a clean single-call stream EXTRACTS — the
                // gemma close marker fired AND common_chat parsed the call, so
                // the terminal directive would register (no longer dropped).
                REQUIRE(r.tool_calls.size() >= 1);
                CHECK(r.tool_calls[0].name.find("read_file") != std::string::npos);
                // Halted AT the close — did not run past it (the consumer shape).
                CHECK(rtrim_ends_with(r.raw_content, kGemmaClose));
                end_test_log();
            }
        }
    }
}
