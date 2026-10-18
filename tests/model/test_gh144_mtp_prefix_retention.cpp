// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh144_mtp_prefix_retention.cpp
 * @brief gh#144: MTP must stop discarding a prefix it can prove unchanged.
 *
 * @par The defect
 * `mtp_init_run` calls `llama_memory_clear(ctx_tgt, true)` and fully
 * re-prefills on EVERY generation, and `generate_mtp` invalidates residency
 * before it ("MTP kernel owns seq 0 itself"). So under `speculative.mtp` the
 * warm-keep path and the prompt cache never execute at all: every turn pays
 * the whole prompt again.
 *
 * That is an implementation choice in the MTP path, not a property of
 * speculative decoding. Nothing about drafting requires discarding a cache.
 *
 * @par Why it matters, measured
 * A consumer's real review workload, three turns, grammar on:
 *
 *     MTP ON    18611 tokens prefilled, 196 generated   (95:1)
 *               0 warm-keep events
 *     MTP OFF    1854 tokens processed, 54843 reused
 *               10 warm-keep events, 98.1% of prefill avoided
 *
 * ~85% of that prefill is a provably invariant prefix — constitution,
 * identity, and a 16 KB staged tool-schema block that does not change between
 * turns. Any host with a real tool surface carries one; it is not a quirk of
 * one consumer.
 *
 * @par RED first
 * This test is written BEFORE the fix and is expected to FAIL, because a test
 * authored afterwards cannot fail for the right reason. On current code
 * prefill CLIMBS with history; once the prefix is retained it should stay
 * near the per-turn delta.
 *
 * Asserts the instrumentation, not the output: a correctness test passes via
 * full-reprefill fallback even when retention is completely dead, which is
 * precisely how the session-pool bug in this same release stayed invisible to
 * 1739 CPU tests.
 *
 * Requires: GPU + gemma4_e2b QAT trunk + mtp_e2b head.
 * Run: ctest -L model -R gh144-mtp
 *
 * @version 2.12.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include "../../src/inference/llama_cpp_backend.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// @brief Models directory, or empty when HOME is unset.
/// @utility
/// @version 2.12.0
fs::path models_dir() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) { return {}; }
    return fs::path(home) / ".entropic" / "models";
}

/// @brief Fixed-size filler so each turn grows history by a constant.
/// @utility
/// @version 2.12.0
std::string filler(const std::string& tag) {
    return tag + ": the quick brown fox jumps over the lazy dog, and then "
           "the dog considers the fox carefully before responding in kind.";
}

}  // namespace

SCENARIO("gh#144: MTP prefill must not re-decode an unchanged prefix",
         "[model][gh144][mtp]")
{
    GIVEN("an MTP-configured backend driven with a growing conversation") {
        auto dir = models_dir();
        REQUIRE_FALSE(dir.empty());
        fs::path trunk = dir / "gemma-4-E4B-it-qat-UD-Q4_K_XL.gguf";
        fs::path head = dir / "mtp-gemma-4-E4B-it.gguf";
        REQUIRE(fs::is_regular_file(trunk));
        REQUIRE(fs::is_regular_file(head));

        entropic::LlamaCppBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = trunk;
        cfg.adapter = "gemma4";
        cfg.context_length = 4096;
        cfg.gpu_layers = 99;
        cfg.flash_attn = true;    // MTP's validated envelope
        cfg.cache_type_k = "q4_0";
        cfg.cache_type_v = "q4_0";
        REQUIRE(backend.load(cfg));
        REQUIRE(backend.activate());

        std::vector<entropic::Message> msgs;
        msgs.push_back({"system",
            "You are a terse assistant. Answer in one short sentence."});
        msgs.push_back({"user", filler("turn 1")});

        entropic::GenerationParams params;
        params.max_tokens = 8;    // output irrelevant — prefill is the signal
        params.temperature = 0.0f;
        params.enable_thinking = false;

        WHEN("four MTP turns run over a growing history") {
            constexpr int kTurns = 4;
            std::atomic<bool> cancel{false};
            std::vector<int> prefill;

            for (int t = 1; t <= kTurns; ++t) {
                backend.generate_mtp(msgs, params, nullptr, cancel,
                                     head.string(), 4);
                prefill.push_back(backend.last_prefill_tokens());
                msgs.push_back({"assistant",
                                filler("reply " + std::to_string(t)),
                                {}, {}});
                msgs.push_back({"user",
                                filler("turn " + std::to_string(t + 1))});
            }

            THEN("prefill stays near the per-turn delta, not the whole prompt") {
                std::string trace;
                for (std::size_t i = 0; i < prefill.size(); ++i) {
                    trace += "turn " + std::to_string(i + 1) + ": "
                             + std::to_string(prefill[i]) + " tok\n";
                }
                INFO("MTP per-turn prefill:\n" << trace);

                REQUIRE(prefill.size() == kTurns);
                CHECK(prefill[0] > 0);

                // RED on current code: mtp_init_run clears and re-prefills
                // every turn, so turn 4 pays the entire accumulated prompt.
                // With the prefix retained it pays only the appended delta,
                // which is constant by construction here.
                for (std::size_t i = 2; i < prefill.size(); ++i) {
                    INFO("turn " << (i + 1) << " prefill " << prefill[i]
                         << " vs turn 2's " << prefill[1]);
                    CHECK(prefill[i] <= prefill[1] + 8);
                }
            }
        }

        backend.deactivate();
        backend.unload();
    }
}
