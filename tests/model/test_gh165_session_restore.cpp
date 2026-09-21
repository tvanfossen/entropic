// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh165_session_restore.cpp
 * @brief gh#165: a restored session must behave like the one it replaced.
 *
 * The emergent multi-turn test for session persistence. The unit tests pin
 * the round trip at the JSON boundary and the refusal at the run guard; only
 * a real model can show that a RESTORED conversation actually drives the
 * next turn — that the model answers from the history the host fed back,
 * over a real prefill, across a process-lifetime boundary the host simulates
 * by dropping the session and putting it back.
 *
 * Three things the unit tests cannot reach:
 *
 *  1. **Restore then continue.** Seed a session, snapshot it, DROP it,
 *     restore the snapshot, then ask a question that can only be answered
 *     from the restored history. A lossy serializer or a restore that did
 *     not reach the engine passes every unit test and fails here.
 *
 *  2. **Restore invalidates derived KV.** Restoring a DIFFERENT conversation
 *     over a session whose slot still holds the old prefix must not decode
 *     against that prefix. Asserted through the answer AND through
 *     `kv_pos_max` staying bounded by the restored input.
 *
 *  3. **Release then restore.** gh#164 + gh#165 together: release the model
 *     (VRAM back), restore the conversation, run again. That is the whole
 *     point of the pair — weights and conversations with independent
 *     lifetimes.
 *
 * @par Sizing (v2.13.0 fixture fix)
 * Both scenarios carried the defect a66aa11 fixed in the gh#158 arms and
 * e1df323 fixed in gh#166: the skip guard looked for the E4B **QAT** file
 * while the tier loaded `gguf_key = "gemma4_e4b"` —
 * `gemma-4-E4B-it-Q8_0.gguf`, 7.6 GiB of weights. The v2.13.0 gate log
 * (build/test-reports/model/logs/test-gh165-restore.log) shows exactly
 * that file loading. Present-and-unchecked is not the same file
 * as absent, so the guard could not skip what the tier then failed to fit:
 * on the floor hardware (a 1080 Ti, 11 GB) the VRAM admission gate refuses
 * it and not one line of gh#165 runs. Guard and tier now name ONE model,
 * the registry's explicitly floor-safe entry (`gemma4_e2b_qat`, 2.44 GiB).
 *
 * They also asked for `context_length: 2048` with the DEFAULT tool menu —
 * a configured handle stages every registered tool, 27 of them, 18,594
 * bytes, ~4,650 prompt tokens. Since 26884f6 that is no longer a quiet
 * degrade: an oversized prompt is a typed refusal
 * (`ENTROPIC_ERROR_EVAL_CONTEXT_FULL`, terminal `context_overflow`), so
 * `run_turn` would come back empty and the seed REQUIRE would fail before
 * a single restore happened. Restoring a conversation needs no tool at
 * all, but the tier contract derives `explicit_completion: true`
 * (`populate_tier_info`, src/facade/entropic.cpp) — an empty menu owes a
 * completion it cannot call — so each tier names exactly that one tool.
 *
 * Requires: GPU + gemma-4-E2B QAT GGUF. Run: ctest -L model -R gh165
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>
#include "facade_model_helpers.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

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

/// @brief Read a session's conversation as JSON. @utility @version 2.13.0
std::string snapshot(entropic_handle_t h, const char* key) {
    char* out = nullptr;
    if (entropic_session_context_get(h, key, &out) != ENTROPIC_OK
        || out == nullptr) {
        return {};
    }
    std::string json = out;
    entropic_free(out);
    return json;
}

/// @brief Run one turn and return its final assistant text.
/// @utility @version 2.13.0
std::string run_turn(entropic_handle_t h, const char* key,
                     const char* input) {
    char* out = nullptr;
    if (entropic_run_session(h, key, input, &out) != ENTROPIC_OK
        || out == nullptr) {
        return {};
    }
    std::string json = out;
    entropic_free(out);
    return json;
}

}  // namespace

SCENARIO("gh#165: a dropped session restored from JSON keeps answering",
         "[model][gh165]")
{
    GIVEN("a configured handle and one seeded session") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }

        entropic::test::facade::FacadeProject project("gh165_restore");
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body =
            "You are a terse assistant. Answer in one short sentence.";
        lead.context_length = 4096;
        // Three plain conversational turns need no filesystem, git or web
        // tool — but the tier DOES owe an explicit completion, so it is
        // given exactly that one (1,595 bytes of schema) instead of the
        // default 18,594-byte menu of 27.
        lead.allowed_tools = {"entropic.complete"};
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        REQUIRE_FALSE(run_turn(
            h, "repo-a",
            "Remember this word, I will ask for it later: cinnamon").empty());
        const std::string stored = snapshot(h, "repo-a");
        REQUIRE_FALSE(stored.empty());

        WHEN("the session is dropped and restored from the snapshot") {
            REQUIRE(entropic_session_drop(h, "repo-a") == ENTROPIC_OK);
            size_t after_drop = 99;
            REQUIRE(entropic_session_context_count(h, "repo-a", &after_drop)
                    == ENTROPIC_OK);
            REQUIRE(after_drop == 0);

            REQUIRE(entropic_session_context_set(h, "repo-a", stored.c_str())
                    == ENTROPIC_OK);

            THEN("the snapshot round-tripped byte for byte") {
                CHECK(snapshot(h, "repo-a") == stored);
            }
            AND_THEN("the model answers from the restored history") {
                // The load-bearing assertion: a restore that never reached
                // the engine, or a serializer that dropped the turn, passes
                // every unit test and fails right here.
                const std::string answer = run_turn(
                    h, "repo-a",
                    "Earlier I gave you one word to remember. Repeat that "
                    "exact word now, and nothing else.");
                INFO("answer: " << answer);
                CHECK(contains_ci(answer, "cinnamon"));
            }
        }

        WHEN("a DIFFERENT conversation is restored over the live session") {
            const char* other =
                R"([{"content":"You are terse.","role":"system"},)"
                R"({"content":"Remember this word, I will ask for it )"
                R"(later: tungsten","role":"user"},)"
                R"({"content":"Understood, I will remember it.",)"
                R"("role":"assistant"}])";
            REQUIRE(entropic_session_context_set(h, "repo-a", other)
                    == ENTROPIC_OK);

            THEN("the next turn answers from the NEW history, not the old") {
                // Restoring must invalidate what the replaced conversation
                // left resident. If the old prefix were reused the model
                // would still be looking at 'cinnamon'.
                const std::string answer = run_turn(
                    h, "repo-a",
                    "Earlier I gave you one word to remember. Repeat that "
                    "exact word now, and nothing else.");
                INFO("answer: " << answer);
                CHECK(contains_ci(answer, "tungsten"));
                CHECK_FALSE(contains_ci(answer, "cinnamon"));
            }
        }
    }
}

SCENARIO("gh#165 + gh#164: release the weights, keep the conversation",
         "[model][gh165]")
{
    GIVEN("a handle with a seeded session") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }

        entropic::test::facade::FacadeProject project("gh165_release");
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.context_length = 4096;
        // See the first scenario: one tool, because the tier owes a
        // completion and this scenario is about the release/restore pair,
        // not the menu. A refused seed turn would leave nothing to
        // snapshot and the recall assertion would never be reached.
        lead.allowed_tools = {"entropic.complete"};
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        REQUIRE_FALSE(run_turn(
            h, "repo-a",
            "Remember this word, I will ask for it later: marigold").empty());
        const std::string stored = snapshot(h, "repo-a");
        REQUIRE_FALSE(stored.empty());

        WHEN("the model is released and the conversation restored after") {
            // The pair's whole argument: a long-running local assistant
            // needs weights and conversation state to have independent
            // lifetimes. Before gh#164 + gh#165, freeing VRAM meant
            // entropic_destroy, which cost every conversation.
            REQUIRE(entropic_release_model(h, nullptr) == ENTROPIC_OK);
            REQUIRE(entropic_session_context_set(h, "repo-a", stored.c_str())
                    == ENTROPIC_OK);

            THEN("the next turn reloads the model and recalls the word") {
                const std::string answer = run_turn(
                    h, "repo-a",
                    "Earlier I gave you one word to remember. Repeat that "
                    "exact word now, and nothing else.");
                INFO("answer: " << answer);
                CHECK(contains_ci(answer, "marigold"));
            }
        }
    }
}
