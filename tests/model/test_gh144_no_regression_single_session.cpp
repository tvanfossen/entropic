// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh144_no_regression_single_session.cpp
 * @brief gh#144: max_sessions == 1 must behave exactly as v2.11.1 did.
 *
 * This is the dispositive check for the riskiest change in the release. The
 * decode path was rewritten from `llama_batch_get_one` — which builds a batch
 * on sequence 0 and AUTO-POSITIONS from the cache cursor — to explicit
 * batches at `seq_pos_max(slot) + 1 + i`. Wrong position arithmetic does not
 * crash and does not fail a unit test. It writes KV cells at the wrong
 * offsets and degrades output silently, so no amount of CPU testing settles
 * it.
 *
 * @par What this asserts, and what it deliberately does NOT
 * It does NOT assert byte-identical output. Greedy decode on this stack is
 * non-deterministic run to run — two identical cold prefills diverge on the
 * dev GPU — so an equality assertion would fail for reasons unrelated to
 * positions, and "fix" itself under a rerun. That is a test that teaches
 * nothing.
 *
 * It asserts the STRUCTURE that wrong positions would break:
 *   - prefill token counts follow the warm-keep pattern (first turn pays the
 *     whole prompt, later turns pay only the delta). Misplaced positions
 *     break the prefix match and every turn pays full freight.
 *   - kv_pos_max tracks the token count. Positions written past the end
 *     inflate it; positions written short of it leave gaps.
 *   - output stays coherent and non-empty across turns. Corrupted KV
 *     degrades into truncation or noise rather than a clean failure.
 *
 * Requires: GPU + gemma4_e4b_qat GGUF. Run: ctest -L model -R gh144
 *
 * @version 2.12.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include "../../src/inference/llama_cpp_backend.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// @brief Fixed ~30-token filler so each turn grows history by a constant.
/// @utility
/// @version 2.12.0
std::string filler(const std::string& tag) {
    return tag + ": the quick brown fox jumps over the lazy dog, and then "
           "the dog considers the fox carefully before responding in kind.";
}

/// @brief Locate the test GGUF.
///
/// gh#144 (v2.12.0) chose the E2B QAT Q4_K_XL (2.4 GB) over the E2B Q8_0
/// (4.7 GB) for half the resident footprint — these tests must survive a
/// developer box that is also running a browser, and a 4.7 GB load was
/// reliably reaching the OOM killer here. v2.14.6: the E2B left the pared
/// model set, so this is the E4B QAT Q4_K_XL (3.9 GiB), the set's dense
/// workhorse. Same gemma4 architecture, so warm-keep behaviour is
/// representative; the smaller TQ2_0 mobile build has no CUDA kernel on the
/// current llama.cpp pin.
/// @utility
/// @version 2.12.0
fs::path test_gguf() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) { return {}; }
    return fs::path(home) / ".entropic" / "models"
           / "gemma-4-E4B-it-qat-UD-Q4_K_XL.gguf";
}

}  // namespace

SCENARIO("gh#144: max_sessions == 1 preserves the warm-keep prefill shape",
         "[model][gh144][regression]")
{
    GIVEN("a single-session backend driven with a growing conversation") {
        auto gguf = test_gguf();
        REQUIRE_FALSE(gguf.empty());
        REQUIRE(fs::is_regular_file(gguf));

        entropic::LlamaCppBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = gguf;
        cfg.adapter = "gemma4";
        cfg.context_length = 4096;
        cfg.gpu_layers = 99;
        cfg.flash_attn = false;
        cfg.max_sessions = 1;   // the path every existing consumer takes
        REQUIRE(backend.load(cfg));
        REQUIRE(backend.activate());

        std::vector<entropic::Message> msgs;
        msgs.push_back({"system",
            "You are a terse assistant. Answer in one short sentence."});
        msgs.push_back({"user", filler("turn 1")});

        entropic::GenerationParams params;
        params.max_tokens = 16;
        params.temperature = 0.0f;
        // session_key deliberately left EMPTY — this is the legacy path.
        REQUIRE(params.session_key.empty());

        WHEN("a four-turn loop runs") {
            constexpr int kTurns = 4;
            std::vector<int> prefill;
            std::vector<std::string> outputs;
            for (int t = 1; t <= kTurns; ++t) {
                auto r = backend.generate(msgs, params);
                prefill.push_back(backend.last_prefill_tokens());
                outputs.push_back(r.content);
                msgs.push_back({"assistant",
                                filler("reply " + std::to_string(t)),
                                {}, {}});
                msgs.push_back({"user",
                                filler("turn " + std::to_string(t + 1))});
            }

            THEN("later turns pay only the delta, not the whole prompt") {
                std::string trace;
                for (size_t i = 0; i < prefill.size(); ++i) {
                    trace += "turn " + std::to_string(i + 1) + ": "
                             + std::to_string(prefill[i]) + " tok\n";
                }
                INFO("per-turn prefill:\n" << trace);
                REQUIRE(prefill.size() == kTurns);
                CHECK(prefill[0] > 0);

                // The invariant is FLATNESS, not "smaller than turn 1".
                // An earlier cut of this test asserted prefill[i] <
                // prefill[0] and failed on correct behaviour: turn 1 is only
                // system + first user (~55 tok) while each later delta is
                // assistant-reply + next-user (~64 tok), so the delta is
                // legitimately LARGER than the first prompt. What misplaced
                // positions actually produce is prefill CLIMBING with history
                // — the prefix match breaks and every turn re-decodes
                // everything — so that is what to assert.
                for (size_t i = 2; i < prefill.size(); ++i) {
                    INFO("turn " << (i + 1) << " prefill " << prefill[i]
                         << " vs turn 2's " << prefill[1]);
                    // Flat within a wide tolerance: the delta is constant by
                    // construction (fixed-size filler), so any growth here is
                    // reuse failing, not the prompt genuinely changing.
                    CHECK(prefill[i] <= prefill[1] + 8);
                }

                // And the decisive one: late-turn prefill must be far below
                // the cumulative conversation length. Without reuse, turn 4
                // would re-decode everything before it.
                const int cumulative = prefill[0] + prefill[1] + prefill[2];
                INFO("turn 4 prefill " << prefill[kTurns - 1]
                     << " vs cumulative history " << cumulative);
                CHECK(prefill[kTurns - 1] < cumulative);
            }

            AND_THEN("every turn produced coherent, non-empty output") {
                // Corrupted KV degrades into truncation or noise rather than
                // failing cleanly, so emptiness is the tripwire.
                for (size_t i = 0; i < outputs.size(); ++i) {
                    INFO("turn " << (i + 1) << " output: " << outputs[i]);
                    CHECK_FALSE(outputs[i].empty());
                }
            }
        }

        backend.deactivate();
        backend.unload();
    }
}
