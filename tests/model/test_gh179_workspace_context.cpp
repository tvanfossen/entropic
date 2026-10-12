// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh179_workspace_context.cpp
 * @brief gh#179: two workspaces, one resident model, each session answers
 *        from ITS workspace's app_context — concurrently, over several turns.
 *
 * The mandated emergent multi-turn model test for a change to what enters
 * a conversation's context. Each repository's project document names a
 * codename that appears nowhere else — not in any path, not in any tool
 * result — so the only way a session can know it is the app_context it was
 * seeded with. Repository A gets its text from the opt-in
 * `workspace_app_context` file; repository B from
 * entropic_workspace_set_app_context. Both run at the same time, and the
 * load-bearing assertions are the NEGATIVE ones: a handle-wide app_context
 * (the pre-v2.14.0 behaviour) would hand both sessions the same codename.
 *
 * Multi-turn on purpose: the codename is asked for again after an
 * unrelated turn, so it must come from the seeded system prompt carried in
 * the accumulated conversation, not from the last message.
 *
 * Requires: GPU, gemma-4-E2B QAT GGUF on disk (smallest bundled model that
 * follows instructions; the property is about context, not capability).
 * Run: inv test --model --filter gh179
 *
 * @version 2.14.0
 */

#include "facade_model_helpers.h"
#include "model_test_context.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

CATCH_REGISTER_LISTENER(FacadeTestListener)

namespace {

namespace fs = std::filesystem;

/// @brief Case-insensitive substring test.
bool has_ci(const std::string& hay, const std::string& needle) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

/// @brief A repository root whose name carries no codename.
fs::path make_repo(const std::string& slot) {
    auto dir = fs::temp_directory_path()
        / ("entropic_gh179_" + slot + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / ".entropic");
    return dir;
}

/// @brief The project document both repositories use, around a codename.
std::string project_doc(const std::string& codename) {
    return "# Project notes\nThis project's internal codename is " + codename
        + ". When asked for the codename, answer with exactly that word.\n";
}

/// @brief The last turn of one session's three-turn conversation.
struct Turns {
    std::string first;       ///< Answer to the first codename question
    std::string last;        ///< Answer to the repeated question
    std::string transcript;  ///< The last turn's whole conversation
};

/// @brief Ask for the codename, chat about something else, ask again.
Turns run_turns(entropic_handle_t h, const std::string& key) {
    using entropic::test::facade::final_answer;
    using entropic::test::facade::run_session_transcript;
    Turns t;
    t.first = final_answer(run_session_transcript(
        h, key.c_str(), "What is this project's codename? One word."));
    run_session_transcript(h, key.c_str(), "Name two fruits, nothing else.");
    t.transcript = run_session_transcript(
        h, key.c_str(), "Repeat this project's codename. One word.");
    t.last = final_answer(t.transcript);
    return t;
}

}  // namespace

SCENARIO("gh#179: each workspace's sessions answer from its own app_context, "
         "concurrently", "[model][gh179][workspace]") {
    GIVEN("one resident model, a handle-wide context, and two repositories") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }
        auto repo_a = make_repo("a");
        auto repo_b = make_repo("b");
        std::ofstream(repo_a / ".entropic" / "ENTROPIC.md") << project_doc("Marigold");

        entropic::test::facade::FacadeProject project("gh179_workspace_context");
        // The handle's own app_context names a THIRD codename: a session that
        // fell back to it is caught by name, not by an absent answer.
        project.extra_config =
            "workspace_app_context: .entropic/ENTROPIC.md\n"
            "app_context:\n  content: \"This project's internal codename is "
            "Tundra.\"\n";
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body = "You are a terse assistant. Answer in one word "
                             "or one short sentence.";
        lead.context_length = 4096;
        // The property is which context a session was seeded with, not
        // completion discipline: with explicit_completion derived true, E2B
        // burned turns on "end with entropic.complete" nudges (flaky on the
        // first gate run). Plain answers carry the codename directly.
        lead.allowed_tools = {"entropic.complete"};
        lead.explicit_completion = false;
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        REQUIRE(entropic_workspace_create(h, "repo-a", repo_a.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_workspace_create(h, "repo-b", repo_b.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_workspace_set_app_context(h, "repo-b",
                    project_doc("Saffron").c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(h, "sess-a", "repo-a") == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(h, "sess-b", "repo-b") == ENTROPIC_OK);

        WHEN("both sessions hold their conversations at the same time") {
            Turns a;
            Turns b;
            std::thread ta([&] { a = run_turns(h, "sess-a"); });
            std::thread tb([&] { b = run_turns(h, "sess-b"); });
            ta.join();
            tb.join();
            INFO("A: [" << a.first << "] / [" << a.last << "]\nA transcript: " << a.transcript);
            INFO("B: [" << b.first << "] / [" << b.last << "]\nB transcript: " << b.transcript);

            THEN("each names its own repository's codename, both times") {
                CHECK(has_ci(a.first, "marigold"));
                CHECK(has_ci(a.last, "marigold"));
                CHECK(has_ci(b.first, "saffron"));
                CHECK(has_ci(b.last, "saffron"));
            }
            THEN("neither saw the other's context or the handle's") {
                // On the TRANSCRIPT, which includes the seeded system prompt:
                // a handle-wide or crossed context lands there whether or not
                // the model repeats it.
                CHECK_FALSE(has_ci(a.transcript, "saffron"));
                CHECK_FALSE(has_ci(b.transcript, "marigold"));
                CHECK_FALSE(has_ci(a.transcript, "tundra"));
                CHECK_FALSE(has_ci(b.transcript, "tundra"));
            }
        }
        fs::remove_all(repo_a);
        fs::remove_all(repo_b);
    }
}
