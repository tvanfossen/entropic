// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh158_concurrent_sessions.cpp
 * @brief gh#158: two sessions running AT THE SAME TIME must not bleed.
 *
 * The mandated emergent multi-turn model test for a change that touches the
 * loop, the tools and the context. gh#144's isolation test interleaves
 * sessions; this one runs them CONCURRENTLY, on two threads, through the
 * public C ABI with `concurrent_sessions: true`. Interleaving cannot catch
 * what this is for: with per-key runs, one session's decode, KV slot,
 * `active_slot_` and staged tool arena are live while another's are — and
 * every one of those was a single per-backend field before
 * `ModelOrchestrator::generation_mutex_`.
 *
 * Four arms:
 *
 *  1. **Cross-session bleed.** Each session is seeded its own secret across
 *     several turns, then asked to recall it while the other is mid-turn.
 *     The load-bearing assertion is the NEGATIVE one — neither session may
 *     recall the other's secret. A shared conversation passes the positive
 *     assertion by accident.
 *
 *  2. **Per-session interrupt.** `entropic_interrupt_session(h, "A")` must
 *     stop A and leave B to finish. Against v2.12.0 there was no such call;
 *     against a naive implementation that reached for the transport latch,
 *     B's in-flight tool call would be aborted too.
 *
 *  3. **Live metrics and context reads.** The audit pass made
 *     `token_counter_`, `per_tier_metrics_` and `last_metrics_` safe; this
 *     arm polls `entropic_metrics_json` and `entropic_context_usage` from a
 *     third thread while two real decodes run, which is what a TUI status
 *     line does every frame.
 *
 *  4. **Hybrid-arch KV.** Every KV-touching change is retested on a
 *     recurrent/hybrid architecture, never on plain-KV gemma alone (the rule
 *     gh#96/gh#97 bought). qwen35moe at gpu_layers=15, asserting the
 *     deterministic desync gate `kv_pos_max < input + max_tokens`.
 *
 * NOT run at pre-commit: GPU, real GGUFs, minutes. This is a G3 gate test.
 *
 * SIZING (v2.13.0). The first three arms carried the defect that broke the
 * gh#166 scenarios: the skip guard looked for the E4B **QAT** file while the
 * tier loaded `gguf_key = "gemma4_e4b"` — `gemma-4-E4B-it-Q8_0.gguf`, 7.6
 * GiB. Present-and-unchecked is not the same file as absent, so the guard
 * could not skip what the tier then failed to fit; it passed G3 only because
 * VRAM happened to be free. Guard and tier now name ONE model, the
 * registry's explicitly floor-safe entry (`gemma4_e2b_qat`, 2.44 GiB).
 *
 * They also ran `context_length: 2048` with the DEFAULT tool menu — all 27
 * registered tools, 18,594 bytes, ~5,000 prompt tokens — which is what
 * produced "Decode chunk failed" on every turn of the G4 run while forty
 * assertions passed anyway. These arms exercise session isolation, not tool
 * use, so the menu names exactly one tool and the prompt fits.
 *
 * NO COMPLETION CONTRACT (v2.13.0). Trimming the menu to
 * `{"entropic.complete"}` paid a debt these arms never owed: a
 * FacadeProject tier derives `explicit_completion: true`
 * (`populate_tier_info`, src/facade/entropic.cpp), so every turn had to end
 * in a tool call — and that call was DENIED at dispatch, because a
 * FacadeProject wrote no `permissions:` block and `auto_approve` defaults
 * to false ("[mcp.tool_executor] No approval callback — denying:
 * entropic.complete", 21 times in
 * build/test-reports/model/logs/test-gh158-concurrent.log). Both sessions
 * recalled their own secret correctly and then spent their remaining turns
 * arguing with "[SYSTEM] ... You must end every turn with exactly one tool
 * call", so `final_answer` — the LAST assistant message — read "I am ready
 * for your next instruction." for A and the bare string "entropic.complete"
 * for B. Each arm now sets `explicit_completion = false`, and FacadeProject
 * writes `permissions.auto_approve: true`.
 *
 * Requires: GPU + gemma-4-E2B QAT GGUF (arms 1-4) and Qwen3.6-35B-A3B
 * (arm 5). Run: ctest -L model -R gh158
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>
#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include "facade_model_helpers.h"
#include "../../src/inference/llama_cpp_backend.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// @brief Case-insensitive substring test. @utility @version 2.13.0
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

/// @brief One session's recall turn, both ways round.
///
/// `entropic_run_session` returns the WHOLE serialized conversation, seed
/// message included, so `CHECK(contains_ci(answer_a, "cinnamon"))` used to
/// match the string this file itself wrote three turns earlier and could
/// not fail. Same defect gh#165 carried; see facade_model_helpers.h. The
/// positive assertions now read `answer`; the negative ones keep reading
/// `transcript`, where another session's seed lands on a bleed whether or
/// not the model repeats it.
/// @version 2.13.0
struct RecallTurn {
    std::string answer;      ///< Final assistant message of the recall turn
    std::string transcript;  ///< That turn's whole serialized conversation
};

/// @brief Seed, filler, recall — three turns on one session. @utility
/// @version 2.13.0
RecallTurn run_session_turns(entropic_handle_t h, const std::string& key,
                             const std::string& secret) {
    using entropic::test::facade::run_session_transcript;
    // Turn 1: seed. Turn 2: unrelated filler, so recall crosses a real turn
    // boundary with accumulated state rather than reading the last message.
    run_session_transcript(
        h, key.c_str(),
        ("Remember this word, I will ask for it later: " + secret).c_str());
    run_session_transcript(h, key.c_str(),
                           "List two colours, nothing else.");
    RecallTurn turn;
    turn.transcript = run_session_transcript(
        h, key.c_str(),
        "Earlier in this conversation I gave you one word to remember. "
        "Repeat that exact word now, and nothing else.");
    turn.answer = entropic::test::facade::final_answer(turn.transcript);
    return turn;
}

}  // namespace

SCENARIO("gh#158: two concurrent sessions do not bleed into each other",
         "[model][gh158]")
{
    GIVEN("one handle with concurrent_sessions enabled") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }

        entropic::test::facade::FacadeProject project("gh158_concurrent");
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body =
            "You are a terse assistant. Answer in one short sentence.";
        lead.context_length = 4096;
        // Three plain conversational turns need no filesystem, git, web or
        // delegation tool. The menu is trimmed to one (1,772 bytes of
        // schema instead of the default 18,594-byte menu of 27) so the
        // prompt fits, and the completion CONTRACT is switched off: this
        // scenario is about which conversation a session recalls from, not
        // about ending a turn with a tool. See the file header.
        lead.allowed_tools = {"entropic.complete"};
        lead.explicit_completion = false;
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        WHEN("two sessions run their turns on two threads at once") {
            RecallTurn a;
            RecallTurn b;
            std::thread ta([&] {
                a = run_session_turns(h, "repo-alpha", "cinnamon");
            });
            std::thread tb([&] {
                b = run_session_turns(h, "repo-bravo", "tungsten");
            });
            ta.join();
            tb.join();

            THEN("each session recalls ITS OWN secret") {
                INFO("A answer: [" << a.answer << "]\nA transcript: "
                     << a.transcript);
                INFO("B answer: [" << b.answer << "]\nB transcript: "
                     << b.transcript);
                CHECK(contains_ci(a.answer, "cinnamon"));
                CHECK(contains_ci(b.answer, "tungsten"));
            }
            AND_THEN("neither recalls the other's — the load-bearing half") {
                // A shared conversation, a shared KV slot or a raced
                // `active_slot_` all show up HERE. Deliberately over the
                // TRANSCRIPT, not the answer: a shared history puts the
                // other session's seed into this session's conversation
                // whether or not the model ever repeats it, and that is the
                // bleed being hunted. The positive half above is the one
                // that has to read the answer — over the transcript it
                // matched this session's own seed and could not fail.
                INFO("A transcript: " << a.transcript);
                INFO("B transcript: " << b.transcript);
                CHECK_FALSE(contains_ci(a.transcript, "tungsten"));
                CHECK_FALSE(contains_ci(b.transcript, "cinnamon"));
            }
            AND_THEN("both sessions are still listed with their own history") {
                char* listed = nullptr;
                REQUIRE(entropic_session_list(h, &listed) == ENTROPIC_OK);
                REQUIRE(listed != nullptr);
                const std::string json = listed;
                entropic_free(listed);
                INFO("sessions: " << json);
                CHECK(json.find("repo-alpha") != std::string::npos);
                CHECK(json.find("repo-bravo") != std::string::npos);
            }
        }
    }
}

SCENARIO("gh#158: interrupting one session leaves the other running",
         "[model][gh158]")
{
    GIVEN("one handle with concurrent_sessions enabled") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }

        entropic::test::facade::FacadeProject project("gh158_interrupt");
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.context_length = 4096;
        // See the first arm: one tool so the prompt fits, no completion
        // contract, because this scenario is about the interrupt. A refused
        // prompt would make the interrupt assertion vacuous — A would never
        // be running when the interrupt arrives.
        lead.allowed_tools = {"entropic.complete"};
        lead.explicit_completion = false;
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        WHEN("A is interrupted by key while B is mid-turn") {
            std::atomic<entropic_error_t> rc_a{ENTROPIC_OK};
            std::atomic<entropic_error_t> rc_b{ENTROPIC_OK};
            std::atomic<bool> a_started{false};

            std::thread ta([&] {
                char* out = nullptr;
                a_started.store(true);
                rc_a.store(entropic_run_session(
                    h, "alpha",
                    "Write a very long essay about tin mining.", &out));
                if (out != nullptr) { entropic_free(out); }
            });
            std::thread tb([&] {
                char* out = nullptr;
                rc_b.store(entropic_run_session(
                    h, "bravo", "Name one colour.", &out));
                if (out != nullptr) { entropic_free(out); }
            });

            while (!a_started.load()) { std::this_thread::yield(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const entropic_error_t irc =
                entropic_interrupt_session(h, "alpha");

            ta.join();
            tb.join();

            THEN("the interrupt found A's run") {
                // NOT_RUNNING here means A had already finished, which makes
                // the rest of the scenario vacuous — surfaced rather than
                // swallowed.
                CHECK(irc == ENTROPIC_OK);
            }
            AND_THEN("B completed on its own terms") {
                CHECK(rc_b.load() == ENTROPIC_OK);
            }
            AND_THEN("interrupting an idle session says so") {
                CHECK(entropic_interrupt_session(h, "nobody")
                      == ENTROPIC_ERROR_NOT_RUNNING);
            }
            (void)rc_a.load();
        }
    }
}

SCENARIO("gh#158: metrics and context reads are safe during live runs",
         "[model][gh158]")
{
    GIVEN("one handle running two sessions at once") {
        // The audit pass of gh#158 made `token_counter_`, `per_tier_metrics_`
        // and `last_metrics_` safe — the first by DELETING an address-keyed
        // memo that a `const` method wrote without a lock, the other two under
        // `metrics_mutex_`. A CPU unit test proves the containers; this proves
        // the real path: a host polling `entropic_metrics_json` and
        // `entropic_context_usage` (which is exactly what a TUI status line
        // does, every frame) while two real decodes are in flight.
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }

        entropic::test::facade::FacadeProject project("gh158_metrics");
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.context_length = 4096;
        // See the first arm. The poller needs two real decodes IN FLIGHT;
        // a refused prompt gives it nothing to race against, and a nudge
        // spiral gives it the wrong thing to race against.
        lead.allowed_tools = {"entropic.complete"};
        lead.explicit_completion = false;
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        WHEN("a poller reads metrics while both sessions decode") {
            std::atomic<bool> done{false};
            std::atomic<int> reads{0};
            std::atomic<int> malformed{0};

            std::thread poller([&] {
                while (!done.load()) {
                    char* json = nullptr;
                    if (entropic_metrics_json(h, &json) == ENTROPIC_OK
                            && json != nullptr) {
                        const std::string text = json;
                        entropic_free(json);
                        // Both keys are unconditional in the envelope, so a
                        // read that lost either one saw a torn object rather
                        // than an empty engine.
                        if (text.find("\"generations\"") == std::string::npos
                                || text.find("\"per_tier\"")
                                    == std::string::npos) {
                            malformed.fetch_add(1);
                        }
                        reads.fetch_add(1);
                    } else {
                        malformed.fetch_add(1);
                    }
                    size_t used = 0;
                    size_t capacity = 0;
                    if (entropic_context_usage(h, &used, &capacity)
                            != ENTROPIC_OK
                            || capacity == 0 || used > capacity) {
                        malformed.fetch_add(1);
                    }
                }
            });

            std::thread ta([&] {
                char* out = nullptr;
                entropic_run_session(h, "m-alpha",
                                     "Name three metals.", &out);
                if (out != nullptr) { entropic_free(out); }
            });
            std::thread tb([&] {
                char* out = nullptr;
                entropic_run_session(h, "m-bravo",
                                     "Name three rivers.", &out);
                if (out != nullptr) { entropic_free(out); }
            });
            ta.join();
            tb.join();
            done.store(true);
            poller.join();

            THEN("every read came back whole") {
                INFO("reads: " << reads.load()
                     << " malformed: " << malformed.load());
                CHECK(reads.load() > 0);
                CHECK(malformed.load() == 0);
            }
        }
    }
}

SCENARIO("gh#158: a batch does not wipe another session's resident KV",
         "[model][gh158]")
{
    GIVEN("a backend with a session pool and one resident session") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }

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

        std::vector<entropic::Message> msgs = {
            {"system", "You are a terse assistant."},
            {"user", "Say the word acorn."},
        };
        entropic::GenerationParams params;
        params.max_tokens = 16;
        params.temperature = 0.0f;
        params.session_key = "keeper";
        REQUIRE(backend.generate(msgs, params).error_code == 0);

        const int clears_before = backend.kv_full_clear_count();

        WHEN("a same-prefix batch runs on the same backend") {
            std::vector<std::vector<entropic::Message>> batch = {
                {{"system", "You are terse."}, {"user", "Say one."}},
                {{"system", "You are terse."}, {"user", "Say two."}},
            };
            std::vector<entropic::GenerationParams> bp(2);
            for (auto& p : bp) { p.max_tokens = 8; p.temperature = 0.0f; }
            std::atomic<bool> cancel{false};
            auto results = backend.generate_batch(batch, bp, cancel);
            const int clears_after = backend.kv_full_clear_count();

            THEN("the batch cleared no session's KV") {
                // The instrumentation assertion. A correctness test passes
                // either way: a wiped session still answers correctly after
                // a silent cold re-prefill, which is exactly the cost the
                // counter exists to make visible.
                INFO("kv full clears before=" << clears_before
                     << " after=" << clears_after);
                CHECK(clears_after == clears_before);
                CHECK(results.size() == 2);
            }
            AND_THEN("the keeper session still decodes from its own prefix") {
                msgs.push_back({"assistant", "acorn"});
                msgs.push_back({"user", "Repeat that word."});
                params.session_key = "keeper";
                auto r = backend.generate(msgs, params);
                INFO("keeper got: " << r.content);
                CHECK(r.error_code == 0);
            }
        }

        backend.deactivate();
        backend.unload();
    }
}

SCENARIO("gh#158: concurrent sessions on a HYBRID arch keep KV in sync",
         "[model][gh158][hybrid]")
{
    GIVEN("the hybrid repro model (Qwen3.6-35B-A3B, arch qwen35moe)") {
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);
        fs::path gguf = fs::path(home) / ".entropic" / "models"
                        / "Qwen3.6-35B-A3B-UD-IQ3_XXS.gguf";
        if (!fs::is_regular_file(gguf)) {
            SKIP("Qwen3.6-35B-A3B GGUF not present at " + gguf.string());
        }

        entropic::LlamaCppBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = gguf;
        cfg.adapter = "qwen36";
        cfg.context_length = 4096;
        cfg.gpu_layers = 15;  // ~13GB IQ3_XXS — OOM-safe partial offload
        cfg.flash_attn = false;
        cfg.max_sessions = 2;
        REQUIRE(backend.load(cfg));
        REQUIRE(backend.activate());

        entropic::GenerationParams params;
        params.max_tokens = 16;
        params.temperature = 0.0f;

        WHEN("two sessions take alternating turns on one recurrent context") {
            struct Turn { int input; int pos_max; std::string content; };
            std::vector<Turn> turns;
            std::vector<std::vector<entropic::Message>> convo = {
                {{"system", "You are a terse assistant."}},
                {{"system", "You are a terse assistant."}},
            };
            const char* keys[] = {"hybrid-a", "hybrid-b"};
            for (int t = 1; t <= 4; ++t) {
                for (int s = 0; s < 2; ++s) {
                    convo[s].push_back(
                        {"user", "Turn " + std::to_string(t)
                                 + ": name one animal."});
                    params.session_key = keys[s];
                    auto r = backend.generate(convo[s], params);
                    turns.push_back({backend.last_input_tokens(),
                                     backend.kv_pos_max(), r.content});
                    convo[s].push_back(
                        {"assistant",
                         r.content.empty() ? std::string("(none)")
                                           : r.content});
                }
            }
            backend.deactivate();
            backend.unload();

            THEN("no turn desyncs the recurrent memory") {
                for (std::size_t i = 0; i < turns.size(); ++i) {
                    INFO("turn " << i << " input=" << turns[i].input
                         << " pos_max=" << turns[i].pos_max
                         << " content=[" << turns[i].content << "]");
                    // The deterministic gate from gh#97: a correct prefill
                    // leaves pos_max ≈ input + generated - 1. An un-removed
                    // tail inflates it past input + max_tokens.
                    CHECK(turns[i].pos_max
                          < turns[i].input + params.max_tokens);
                }
            }
        }
    }
}
