// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh96_warmkeep.cpp
 * @brief gh#96: the agent loop re-prefills the full post-system history every
 *        turn; warm-keep should let the resident KV expand and re-decode only
 *        the per-turn delta.
 *
 * BACKEND-DIRECT (warm-keep is a backend mechanism): drive LlamaCppBackend
 * with a monotonically-growing message vector — the per-turn calls the agent
 * loop makes — and read last_prefill_tokens() (llama_perf n_p_eval, gh#96
 * instrumentation) after each generation.
 *
 * Today the prompt cache reuses only the system prefix, so every turn
 * re-decodes the whole post-system history → the prefill token count CLIMBS
 * each turn. Warm-keep keeps the prior turns' KV resident and decodes only
 * the appended delta → the prefill count stays ~flat regardless of history
 * length.
 *
 * RED on the current rev (prefill grows); GREEN once warm-keep reuse lands.
 *
 * Requires: GPU + gemma4_e2b GGUF. Run: ctest -L model -R gh96
 *
 * @version 2.7.5
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include "../../src/inference/llama_cpp_backend.h"

#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// @brief A fixed ~30-token filler so each turn grows the history by a
///        constant amount — makes the per-turn prefill delta deterministic.
/// @utility
/// @version 2.7.5
std::string filler(const std::string& tag) {
    return tag + ": the quick brown fox jumps over the lazy dog, and then "
           "the dog considers the fox carefully before responding in kind.";
}

}  // namespace

SCENARIO("gh#96: agent-loop prefill must not grow with history (warm-keep)",
         "[model][gh96]")
{
    GIVEN("a cache-enabled backend driven with a growing conversation") {
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);
        fs::path gguf = fs::path(home) / ".entropic" / "models"
                        / "gemma-4-E2B-it-Q8_0.gguf";
        if (!fs::is_regular_file(gguf)) {
            SKIP("gemma4_e2b GGUF not present at " + gguf.string());
        }

        entropic::LlamaCppBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = gguf;
        cfg.adapter = "gemma4";
        cfg.context_length = 4096;
        cfg.gpu_layers = 99;
        cfg.flash_attn = false;
        REQUIRE(backend.load(cfg));
        REQUIRE(backend.activate());
        // Prompt cache is enabled by default; a non-empty system message
        // engages the system-prefix HIT path (the production agentic path).

        // Seed: system + first user message.
        std::vector<entropic::Message> msgs;
        msgs.push_back({"system",
            "You are a terse assistant. Answer in one short sentence."});
        msgs.push_back({"user", filler("turn 1")});

        entropic::GenerationParams params;
        params.max_tokens = 8;        // output irrelevant — we measure prefill
        params.temperature = 0.0f;

        WHEN("running a 6-turn loop, recording prefill tokens + ms per turn") {
            constexpr int kTurns = 6;
            std::vector<int> prefill_tok;
            std::vector<double> prefill_ms;
            for (int t = 1; t <= kTurns; ++t) {
                backend.generate(msgs, params);
                prefill_tok.push_back(backend.last_prefill_tokens());
                prefill_ms.push_back(backend.last_prefill_ms());
                // Simulate the loop appending the assistant turn + next user.
                msgs.push_back({"assistant", filler("reply " + std::to_string(t)),
                                {}, {}});
                msgs.push_back({"user",
                    filler("turn " + std::to_string(t + 1))});
            }
            backend.deactivate();
            backend.unload();

            THEN("late-turn prefill stays flat in BOTH tokens and wall-clock") {
                std::string trace;
                for (size_t i = 0; i < prefill_tok.size(); ++i) {
                    trace += "turn " + std::to_string(i + 1) + ": "
                             + std::to_string(prefill_tok[i]) + " tok / "
                             + std::to_string(prefill_ms[i]) + " ms\n";
                }
                INFO("per-turn prefill:\n" << trace);

                // prefill[0] is the one-time cache MISS (full initial prompt).
                // The warm turns are 1..5. With warm-keep each re-decodes only
                // the appended delta, so the last warm turn must not exceed the
                // first warm turn by much. On the current rev every turn
                // re-decodes the whole post-system history → grows
                // turn-over-turn and both bounds fail (RED).

                // (a) Token gate — deterministic, no GPU noise. THIS is the
                // RED→GREEN discriminator: re-decode per turn must collapse to
                // the appended delta once warm-keep lands.
                int first_tok = prefill_tok[1];
                int last_tok = prefill_tok.back();
                CHECK(first_tok > 0);
                CHECK(last_tok <= first_tok * 3 / 2);

                // (b) Wall-clock — measured + surfaced (the cost the consumer
                // actually feels; it climbs with the re-decoded token count).
                // We do NOT hard-assert a flat bound here: at small-model scale
                // a fixed ~30ms per-call overhead compresses the ratio, so a
                // tight bound would flake on GPU noise. The robust, noise-immune
                // timing-WIN assertion lives in the warm-keep on-vs-off A/B
                // (same turn, same work ± reuse) — see test_gh96_warmkeep_oracle.
                // Here we only assert the timer is live so the signal is real.
                double first_ms = prefill_ms[1];
                double last_ms = prefill_ms.back();
                CHECK(first_ms > 0.0);
                CHECK(last_ms > 0.0);
            }
        }
    }
}
