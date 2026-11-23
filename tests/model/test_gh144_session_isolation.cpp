// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh144_session_isolation.cpp
 * @brief gh#144: three sessions on one engine must not see each other.
 *
 * The mandated multi-turn model test: real model, persistent context,
 * accumulated state, sessions INTERLEAVED rather than run to completion one
 * at a time. Single-turn coverage would pass even if the KV were shared,
 * because a single turn has no prior state to leak.
 *
 * Each session is seeded a distinct secret and later asked to recall it.
 * Against v2.11.1 all three shared one conversation, so a session would
 * recall whichever secret was seeded last.
 *
 * Runs under the consumer's real configuration — session pool of three at
 * 32k, q4_0 K/V — because that is the combination least exercised in
 * llama.cpp: non-unified KV plus interleaved sliding-window attention.
 *
 * Requires: GPU + gemma4_e4b_qat GGUF. Run: ctest -L model -R gh144
 *
 * @version 2.12.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include "../../src/inference/llama_cpp_backend.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// @brief Locate the test GGUF.
///
/// gh#144 (v2.12.0): the ISOLATION test uses E4B QAT Q4_K_XL (3.9 GB) —
/// the consumer's own model — because three-way cross-turn recall is beyond
/// the E2B: it recalled one session's secret and produced "one" or
/// "Understood, I will repeat the..." for the other two. A model that cannot
/// recall makes the cross-contamination assertions pass VACUOUSLY, which is
/// worse than a failure. v2.14.6: the E2B left the pared model set, so the
/// other gh#144 tests now use this same E4B QAT Q4_K_XL too.
/// @utility
/// @version 2.12.0
fs::path test_gguf() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) { return {}; }
    return fs::path(home) / ".entropic" / "models"
           / "gemma-4-E4B-it-qat-UD-Q4_K_XL.gguf";
}

/// @brief Case-insensitive substring test.
/// @utility
/// @version 2.12.0
bool contains_ci(const std::string& hay, const std::string& needle) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

/// @brief One session's running conversation.
/// @version 2.12.0
struct Session {
    std::string key;                            ///< Session key
    std::string secret;                         ///< Word only this one knows
    std::vector<entropic::Message> msgs;        ///< Its own history
};

}  // namespace

SCENARIO("gh#144: interleaved sessions do not leak context to each other",
         "[model][gh144]")
{
    GIVEN("one backend serving three sessions") {
        auto gguf = test_gguf();
        REQUIRE_FALSE(gguf.empty());
        REQUIRE(fs::is_regular_file(gguf));

        entropic::LlamaCppBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = gguf;
        cfg.adapter = "gemma4";
        cfg.context_length = 2048;   // per session; the pool multiplies it
        cfg.gpu_layers = 99;
        cfg.flash_attn = false;
        cfg.max_sessions = 3;
        REQUIRE(backend.load(cfg));
        REQUIRE(backend.activate());

        std::vector<Session> sessions = {
            {"repo-alpha", "cinnamon", {}},
            {"repo-bravo", "tungsten", {}},
            {"repo-delta", "marigold", {}},
        };
        for (auto& s : sessions) {
            s.msgs.push_back({"system",
                "You are a terse assistant. Answer in one short sentence."});
        }

        entropic::GenerationParams params;
        // gh#144: a 24-token budget with thinking ON produced
        // "<|channel>thought" and truncated before any answer, so recall
        // failed and the cross-contamination assertions then passed
        // VACUOUSLY — an output of "word" trivially contains no other
        // session's secret, and would pass with fully shared context too.
        // The isolation claim only means something once recall works.
        params.max_tokens = 64;
        params.enable_thinking = false;
        params.temperature = 0.0f;

        WHEN("each is seeded a secret, interleaved, then asked to recall it") {
            // Seed round — A, B, C in turn, never one session to completion.
            for (auto& s : sessions) {
                s.msgs.push_back({"user",
                    "Remember this word, I will ask for it later: "
                    + s.secret});
                params.session_key = s.key;
                backend.generate(s.msgs, params);
                // Store a CANNED acknowledgement rather than the model's own
                // reply. Feeding a 2B model's seed-turn output back made the
                // conversation incoherent for two of three sessions, which
                // then failed recall for reasons having nothing to do with
                // isolation. The subject under test is whether session B can
                // see session A's history — not whether a small model writes
                // a good acknowledgement.
                s.msgs.push_back({"assistant",
                                  "Understood, I will remember it.", {}, {}});
            }

            // Recall round — same interleaving.
            std::vector<std::string> answers;
            for (auto& s : sessions) {
                s.msgs.push_back({"user",
                    "Earlier in this conversation I gave you one word to "
                    "remember. Repeat that exact word now, and nothing "
                    "else."});
                params.session_key = s.key;
                auto r = backend.generate(s.msgs, params);
                answers.push_back(r.content);
                s.msgs.push_back({"assistant", r.content, {}, {}});
            }

            THEN("each session recalls ITS OWN secret") {
                for (std::size_t i = 0; i < sessions.size(); ++i) {
                    INFO("session " << sessions[i].key
                         << " expected '" << sessions[i].secret
                         << "' got: " << answers[i]);
                    CHECK(contains_ci(answers[i], sessions[i].secret));
                }
            }

            AND_THEN("no session recalls another's secret") {
                // The load-bearing half. Against one shared conversation
                // every session saw all three seeds, so this is what
                // actually distinguishes isolation from coincidence.
                for (std::size_t i = 0; i < sessions.size(); ++i) {
                    for (std::size_t j = 0; j < sessions.size(); ++j) {
                        if (i == j) { continue; }
                        INFO("session " << sessions[i].key
                             << " must not know " << sessions[j].secret
                             << " — got: " << answers[i]);
                        CHECK_FALSE(
                            contains_ci(answers[i], sessions[j].secret));
                    }
                }
            }
        }

        backend.deactivate();
        backend.unload();
    }
}
