// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_outside_root_approval.cpp
 * @brief v2.13.0: a real model reading outside its root, one approval and
 *        one rejection, across a persistent multi-turn conversation.
 *
 * The mandated emergent multi-turn model test for a change that touches
 * tools and context. `mcp.filesystem.allow_outside_root: optional` sends
 * every path outside the project root to the host's path approver. Two
 * files sit OUTSIDE the root; the approver ACCEPTS the first and REJECTS
 * the second — keyed by path, not by call order, so a model that re-reads
 * the first file is approved again rather than silently flipping the test.
 *
 * Three turns on ONE conversation:
 *   1. read the first file  → approved → its codeword reaches the model;
 *   2. read the second file → rejected → the model receives the typed
 *      `outside_root_rejected` refusal and NOTHING of the file;
 *   3. recall the first codeword → answered from accumulated context.
 *
 * The load-bearing assertions are engine-side and deterministic: what the
 * approver was asked (path, READ, tool), and what the tool results carried
 * (the served codeword, the refusal text, and the rejected file's codeword
 * appearing NOWHERE in the transcript). The one positive claim about the
 * model's own words is the turn-3 recall, read from the final answer —
 * never from the transcript, which already contains the turn-1 tool result
 * and would pass whatever the model said (the gh#165 trap).
 *
 * File names and codewords are DECOUPLED (`alpha.txt` holds PLOVER): a
 * refusal message names the path, so a codeword in a path would make the
 * negative assertion fail on containment WORKING (the gh#166 lesson).
 *
 * `allow_outside_root: optional` is set EXPLICITLY in the project layer:
 * `entropic_configure_dir` layers ~/.entropic/config.yaml underneath, and
 * a global file auto-created from a pre-2.13 bundled default says `true`,
 * which would serve both files without ever asking the approver.
 *
 * Requires: GPU, gemma-4-E2B QAT GGUF on disk.
 * Run: ctest -L model -R outside-root-approval
 *
 * @version 2.13.0
 */

#include "facade_model_helpers.h"
#include "model_test_context.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

CATCH_REGISTER_LISTENER(ModelTestListener)

namespace {

namespace fs = std::filesystem;

/// @brief Case-insensitive substring test. @utility @version 2.13.0
bool has_ci(const std::string& hay, const std::string& needle) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

/**
 * @brief The host side: approves exactly one path, records every request.
 *
 * The callback fires on the run thread; the mutex makes the record safe
 * to read from the test thread afterwards regardless.
 * @version 2.13.0
 */
struct PathApprover {
    std::string approve;                 ///< The one canonical path to ACCEPT
    std::mutex mu;                       ///< Guards the record
    std::vector<std::string> paths;      ///< Every path asked about
    std::vector<std::string> tools;      ///< Every tool reported
    std::vector<ent_path_access_t> access; ///< Every access kind
    int accepted = 0;                    ///< ACCEPT count
    int rejected = 0;                    ///< REJECT count

    /** @brief C trampoline. @utility @version 2.13.0 */
    static ent_decision_t call(const ent_path_approval_request_t* req,
                               void* ud) {
        auto* self = static_cast<PathApprover*>(ud);
        std::lock_guard<std::mutex> lock(self->mu);
        self->paths.emplace_back(req->path);
        self->tools.emplace_back(req->tool);
        self->access.push_back(req->access);
        const bool ok = self->approve == req->path;
        (ok ? self->accepted : self->rejected) += 1;
        return ok ? ENT_DECISION_ACCEPT : ENT_DECISION_REJECT;
    }

    /** @brief Whether any request named `p`. @utility @version 2.13.0 */
    bool asked_about(const fs::path& p) {
        std::lock_guard<std::mutex> lock(mu);
        return std::find(paths.begin(), paths.end(), p.string())
               != paths.end();
    }
};

}  // namespace

SCENARIO("outside-root optional: a real model's reads outside its root "
         "follow the host approver's decisions across turns",
         "[model][outside_root][v2.13.0]")
{
    GIVEN("a project root, two files outside it, and an approver that "
          "accepts one") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }

        auto outside = fs::weakly_canonical(fs::temp_directory_path()) /
            ("entropic_outside_root_" + std::to_string(::getpid()));
        fs::remove_all(outside);
        fs::create_directories(outside);
        const auto alpha = outside / "alpha.txt";
        const auto bravo = outside / "bravo.txt";
        std::ofstream(alpha) << "The codeword in this file is PLOVER.\n";
        std::ofstream(bravo) << "The codeword in this file is QUOKKA.\n";

        entropic::test::facade::FacadeProject project("outside_root_approval");
        project.extra_config =
            "mcp:\n"
            "  working_dir: " + project.dir().string() + "\n"
            "  filesystem:\n"
            "    allow_outside_root: optional\n";

        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body =
            "You are a terse assistant with a filesystem.read_file tool. "
            "When asked about a file, call filesystem.read_file with the "
            "exact absolute path you were given, then answer in one short "
            "sentence. If a tool call fails, say that it failed.";
        lead.context_length = 4096;
        // One reader plus the completion the tier owes (explicit_completion
        // derives true for a FacadeProject tier — see gh#166's test).
        lead.allowed_tools = {"filesystem.read_file", "entropic.complete"};
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        PathApprover approver;
        approver.approve = alpha.string();
        REQUIRE(entropic_set_path_approval_callback(
                    h, &PathApprover::call, &approver) == ENTROPIC_OK);

        WHEN("the model reads the approved file, then the rejected one, "
             "then recalls the first") {
            using entropic::test::facade::final_answer;
            using entropic::test::facade::run_transcript;
            auto t1 = run_transcript(h, (
                "Use filesystem.read_file on the absolute path "
                + alpha.string() + " and tell me the codeword it contains.")
                .c_str());
            auto t2 = run_transcript(h, (
                "Now use filesystem.read_file on the absolute path "
                + bravo.string() + " and tell me its codeword.").c_str());
            auto t3 = run_transcript(
                h, "What was the codeword from the FIRST file you read? "
                   "Answer with that one word.");
            auto answer3 = final_answer(t3);

            // ONE leaf on purpose: Catch2 re-runs GIVEN/WHEN per THEN, and
            // each re-run is a model load plus three turns.
            THEN("each tool result, and the recall, follow the decisions") {
                INFO("turn 1 transcript: " << t1);
                INFO("turn 2 transcript: " << t2);
                INFO("turn 3 answer: [" << answer3 << "]\ntranscript: "
                     << t3);

                // The approver was asked about BOTH files, as READs.
                CHECK(approver.asked_about(alpha));
                CHECK(approver.asked_about(bravo));
                {
                    std::lock_guard<std::mutex> lock(approver.mu);
                    CHECK(approver.accepted >= 1);
                    CHECK(approver.rejected >= 1);
                    for (size_t i = 0; i < approver.tools.size(); ++i) {
                        CHECK(approver.tools[i] == "filesystem.read_file");
                        CHECK(approver.access[i] == ENT_PATH_ACCESS_READ);
                    }
                }

                // Turn 1: the approved read's content reached the model.
                CHECK(has_ci(t1, "PLOVER"));

                // Turn 2: the rejection reached the model as a typed
                // refusal and NONE of the file did. The negative is over
                // the whole transcript — a served read lands there as a
                // tool result whether or not the model repeats it.
                CHECK(t2.find("outside_root_rejected") != std::string::npos);
                CHECK_FALSE(has_ci(t2, "QUOKKA"));
                CHECK_FALSE(has_ci(t3, "QUOKKA"));

                // Turn 3: recalled from accumulated context, read from the
                // ANSWER — the transcript already holds turn 1's result.
                CHECK(has_ci(answer3, "PLOVER"));
            }
        }

        fs::remove_all(outside);
    }
}
