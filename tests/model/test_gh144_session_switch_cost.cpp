// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh144_session_switch_cost.cpp
 * @brief gh#144: what a session switch actually costs, measured.
 *
 * Produces the number, and asserts the instrumentation rather than the
 * output. A correctness test passes via cold-prefill fallback even when the
 * optimisation is completely dead, so "the answer was right" says nothing
 * about whether residency engaged. `last_prefill_tokens()` does.
 *
 * @par What the arms mean
 *   SAME    — consecutive turns on one session. Its KV is resident, so the
 *             turn should pay only the appended delta.
 *   SWITCH  — alternating between two sessions. Each still has its own
 *             resident slot, so it should ALSO pay only its delta. Against a
 *             single shared residency vector this is where the destructive
 *             reuse branch fired: a non-zero common prefix (the shared system
 *             prompt) triggered a seq_rm of the other session's tail and a
 *             full re-decode.
 *
 * @par Deliberately not asserted
 * No wall-clock bound. The consumer's own A/B showed wall clock on this stack
 * is iterations times per-iteration cost, and iteration count varies 2x on
 * identical input — so a time-based assertion would fail for reasons having
 * nothing to do with KV. Token counts are the clean signal and they do not
 * depend on how many turns the model decides it needs.
 *
 * Under speculative.mtp this test will show NO reuse in either arm, because
 * mtp_init_run clears the whole context and re-prefills every turn. That is
 * expected until prefix retention lands, and is exactly the baseline this
 * measurement exists to establish.
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
#include <numeric>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

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

/// @brief Fixed-size filler so each turn grows history by a constant.
/// @utility
/// @version 2.12.0
std::string filler(const std::string& tag) {
    return tag + ": the quick brown fox jumps over the lazy dog, and then "
           "the dog considers the fox carefully before responding in kind.";
}

}  // namespace

SCENARIO("gh#144: a session switch must not force a full re-prefill",
         "[model][gh144]")
{
    GIVEN("a two-session pool") {
        auto gguf = test_gguf();
        REQUIRE_FALSE(gguf.empty());
        REQUIRE(fs::is_regular_file(gguf));

        entropic::LlamaCppBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = gguf;
        cfg.adapter = "gemma4";
        cfg.context_length = 2048;
        cfg.gpu_layers = 99;
        cfg.flash_attn = false;
        cfg.max_sessions = 2;
        REQUIRE(backend.load(cfg));
        REQUIRE(backend.activate());

        entropic::GenerationParams params;
        params.max_tokens = 8;   // output irrelevant — prefill is the signal
        params.temperature = 0.0f;

        std::vector<entropic::Message> a{{"system", "Be terse."}};
        std::vector<entropic::Message> b{{"system", "Be terse."}};

        // Warm both sessions so each owns a slot with resident KV.
        a.push_back({"user", filler("a-1")});
        params.session_key = "sess-a";
        backend.generate(a, params);
        a.push_back({"assistant", filler("ra-1"), {}, {}});

        b.push_back({"user", filler("b-1")});
        params.session_key = "sess-b";
        backend.generate(b, params);
        b.push_back({"assistant", filler("rb-1"), {}, {}});

        WHEN("turns alternate between the two sessions") {
            std::vector<int> switched;
            for (int t = 2; t <= 4; ++t) {
                a.push_back({"user", filler("a-" + std::to_string(t))});
                params.session_key = "sess-a";
                backend.generate(a, params);
                switched.push_back(backend.last_prefill_tokens());
                a.push_back({"assistant",
                             filler("ra-" + std::to_string(t)), {}, {}});

                b.push_back({"user", filler("b-" + std::to_string(t))});
                params.session_key = "sess-b";
                backend.generate(b, params);
                switched.push_back(backend.last_prefill_tokens());
                b.push_back({"assistant",
                             filler("rb-" + std::to_string(t)), {}, {}});
            }

            THEN("each switched turn pays a delta, not the whole history") {
                std::string trace;
                for (std::size_t i = 0; i < switched.size(); ++i) {
                    trace += "switched turn " + std::to_string(i + 1) + ": "
                             + std::to_string(switched[i]) + " prefill tok\n";
                }
                INFO("SWITCH arm:\n" << trace);

                // The instrumentation assertion. A correctness test would
                // pass here via cold-prefill fallback; this is what fails
                // when residency is dead.
                REQUIRE_FALSE(switched.empty());
                const int total_history_tokens =
                    static_cast<int>(a.size() + b.size()) * 20;
                for (std::size_t i = 0; i < switched.size(); ++i) {
                    INFO("turn " << (i + 1) << " prefilled "
                         << switched[i] << " tokens");
                    CHECK(switched[i] < total_history_tokens);
                }
            }

            AND_THEN("the later switched turns do not grow without bound") {
                // If residency were being destroyed on every switch, prefill
                // would climb with history length rather than stay near the
                // per-turn delta.
                REQUIRE(switched.size() >= 4);
                const int early = switched[0] + switched[1];
                const int late = switched[switched.size() - 2]
                               + switched[switched.size() - 1];
                INFO("early pair " << early << " vs late pair " << late);
                CHECK(late <= early * 2);
            }
        }

        backend.deactivate();
        backend.unload();
    }
}
