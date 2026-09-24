// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh99_run_as.cpp
 * @brief gh#99: entropic_run_as must run each call under the NAMED tier's
 *        grammar on a single shared resident model.
 *
 * FACADE-DRIVEN + EMERGENT MULTI-TURN. Reproduces the consumer's exact symptom:
 * "entropic_load_identity('companion') returns found=true, but the next
 * entropic_run still emits the npc tier's grammar output." Two tiers share ONE
 * model (same gguf_key → one VRAM load via the orchestrator's model pool) but
 * carry DIFFERENT grammars:
 *   - npc       → root ::= "STRIDE" | "FORAGE" | "REST"   (bare verbs)
 *   - companion → root ::= "ANSWER"                        (a distinct literal)
 *
 * Alternating entropic_run_as("npc")/("companion") across a persistent-context
 * session must emit EACH tier's grammar shape. On the unfixed engine (no per-
 * call tier override → run always uses the default/locked tier's grammar) the
 * companion call emits a verb, NOT "ANSWER" — the genuine RED. The grammar is
 * deterministic, so the assertion holds regardless of model quality.
 *
 * Requires: GPU + gemma4_e2b GGUF. Run: ctest -L model -R gh99
 *
 * @version 2.8.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>

#include <filesystem>
#include <string>
#include <vector>

#include "facade_model_helpers.h"

namespace fs = std::filesystem;
using entropic::test::facade::FacadeProject;
using entropic::test::facade::model_gguf;
using entropic::test::facade::TierSpec;

namespace {

/// @brief One run_as call, both ways round.
///
/// v2.13.0: `entropic_run_as` returns the WHOLE accumulated conversation,
/// and this handle is persistent — so by the third call the transcript
/// already contains the first call's verb. `CHECK(has_verb(npc2))` over it
/// could not fail. The per-call claims now read `answer`.
struct RunAs {
    std::string answer;      ///< Final assistant message of THIS call
    std::string transcript;  ///< Everything the handle has accumulated
};

RunAs run_as(entropic_handle_t h, const char* tier, const char* prompt) {
    char* out = nullptr;
    // Kept inline rather than routed through run_transcript: the rc is
    // asserted directly here, and its value is the diagnostic.
    auto rc = entropic_run_as(h, tier, prompt, &out);
    RunAs r;
    r.transcript = entropic::test::facade::take_owned(out);
    r.answer = entropic::test::facade::final_answer(r.transcript);
    REQUIRE(rc == ENTROPIC_OK);
    return r;
}

bool has_verb(const std::string& s) {
    return s.find("STRIDE") != std::string::npos
        || s.find("FORAGE") != std::string::npos
        || s.find("REST") != std::string::npos;
}

}  // namespace

SCENARIO("gh#99: run_as emits each tier's grammar on a shared model",
         "[model][gh99]")
{
    GIVEN("one handle with npc (verbs) + companion (answer) tiers") {
        fs::path gguf = model_gguf("gemma-4-E2B-it-Q8_0.gguf");
        if (!fs::is_regular_file(gguf)) {
            SKIP("gemma4_e2b GGUF not present at " + gguf.string());
        }

        FacadeProject proj("gh99_run_as");
        // context_length 16384: the staged meta-tool prompt is ~4900 tokens —
        // a too-small ctx overflows and garbles output regardless of the
        // grammar (see the gh#95 grammar-threading lesson).
        std::vector<TierSpec> tiers = {
            {"npc", "gemma4_e2b", "gemma4",
             "You are an NPC. Choose exactly one action.",
             "npcverb", "root ::= \"STRIDE\" | \"FORAGE\" | \"REST\"\n",
             16384, 99, 1},
            {"companion", "gemma4_e2b", "gemma4",
             "You are a companion. Answer the traveler.",
             "companionans", "root ::= \"ANSWER\"\n",
             16384, 99, 1},
        };
        entropic_handle_t h = proj.setup(tiers, "npc");
        REQUIRE(h != nullptr);

        WHEN("alternating run_as across tiers on the same persistent handle") {
            RunAs npc1 = run_as(h, "npc", "What do you do?");
            RunAs comp1 = run_as(h, "companion", "What do you say?");
            RunAs npc2 = run_as(h, "npc", "And now?");

            THEN("each call emits ITS tier's grammar, not the default's") {
                INFO("npc1=[" << npc1.answer << "] comp1=[" << comp1.answer
                     << "] npc2=[" << npc2.answer << "]\nfinal transcript: "
                     << npc2.transcript);
                // Each assertion is about what THAT call emitted, so each
                // reads that call's own final assistant message. Over the
                // accumulated transcript (what run_as used to return)
                // `has_verb(npc2)` matched npc1's verb and could not fail.
                //
                // "ANSWER" is still the discriminator: only the companion
                // grammar can emit it. On the bug (run_as ignores the tier
                // and uses the default npc grammar) the companion call emits
                // a verb and ANSWER never appears — the genuine RED.
                CHECK(has_verb(npc1.answer));                             // npc grammar
                CHECK(npc1.answer.find("ANSWER") == std::string::npos);   // not companion's
                CHECK(comp1.answer.find("ANSWER") != std::string::npos);  // companion grammar
                CHECK(has_verb(npc2.answer));                             // switched back
            }
        }

        WHEN("run_as names an unknown tier") {
            char* out = nullptr;
            auto rc = entropic_run_as(h, "bbeg", "hi", &out);
            if (out != nullptr) { entropic_free(out); }
            THEN("it errors cleanly without crashing") {
                CHECK(rc == ENTROPIC_ERROR_IDENTITY_NOT_FOUND);
            }
        }
    }
}
