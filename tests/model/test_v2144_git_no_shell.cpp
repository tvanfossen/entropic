// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v2144_git_no_shell.cpp
 * @brief gh#211 (v2.14.4): a real model's git calls never reach a shell.
 *
 * The emergent multi-turn test for the argv-based git server. The user asks
 * the model to stage a file and commit it with a message that contains
 * double quotes, a `$(...)` command substitution and backticks — text that
 * the pre-2.14.4 popen() path would have EXECUTED (the substitution touches
 * a marker file). A later turn asks for the log. The message must land in
 * history verbatim and the marker must never exist.
 *
 * Requires: GPU, gemma-4-E2B QAT GGUF on disk.
 * Run: inv test --model --model-only --filter v2144
 *
 * @version 2.14.4
 */

#include "facade_model_helpers.h"
#include "model_test_context.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

CATCH_REGISTER_LISTENER(FacadeTestListener)

namespace fs = std::filesystem;

SCENARIO("v2.14.4: a model's commit message with shell syntax is text, not a command",
         "[model][v2144][git][security]") {
    GIVEN("a git repository the model's git tools are rooted at") {
        auto gguf = entropic::test::facade::model_gguf("gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }
        const auto repo = fs::temp_directory_path() / ("entropic_v2144_" + std::to_string(::getpid()));
        fs::remove_all(repo);
        fs::create_directories(repo);
        const std::string q = "'" + repo.string() + "'";
        std::system(("git -C " + q + " init -q && git -C " + q + " config user.email t@t"
                     " && git -C " + q + " config user.name T").c_str());
        std::ofstream(repo / "notes.txt") << "hello\n";
        const auto marker = repo.parent_path() / ("release-stamp-" + std::to_string(::getpid()));
        fs::remove(marker);

        entropic::test::facade::FacadeProject project("v2144_git_no_shell");
        project.mcp_working_dir = repo;
        project.extra_mcp_config = "  enable_git: true\n";
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body = "You are a careful assistant with git tools. When asked "
                             "to commit, pass the user's message to git.commit exactly "
                             "as given, character for character.";
        lead.context_length = 4096;
        lead.allowed_tools = {"git.add", "git.commit", "git.log"};
        lead.explicit_completion = false;
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        const std::string message = "Add notes \"v1\" $(touch " + marker.string()
                                  + ") `date`";
        using entropic::test::facade::run_session_transcript;
        // One tool step per turn: E2B does not reliably chain add -> commit
        // inside a single turn, and chaining is not the property under test.
        run_session_transcript(h, "s", "Stage notes.txt with git.add.");
        run_session_transcript(h, "s",
            ("Now commit with git.commit using exactly this message: " + message).c_str());
        run_session_transcript(h, "s", "Name one colour, nothing else.");
        const auto transcript = run_session_transcript(h, "s",
            "Show the git log with git.log, count 1.");

        // ONE THEN on purpose: Catch2 re-runs the whole GIVEN per THEN, and a
        // second model conversation doubles the chance it skips the commit.
        THEN("the commit landed with its shell syntax as text, and nothing ran") {
            CHECK_FALSE(fs::exists(marker));
            const std::string out_file = (repo.parent_path() / "v2144_log.txt").string();
            std::system(("git -C " + q + " log -1 --format=%s > '" + out_file + "' 2>&1").c_str());
            std::ifstream in(out_file);
            const std::string subject((std::istreambuf_iterator<char>(in)), {});
            INFO("subject: " << subject << "\ntranscript: " << transcript);
            CHECK(subject.find("$(touch") != std::string::npos);
            CHECK(subject.find("`date`") != std::string::npos);
            fs::remove(out_file);
        }
        fs::remove_all(repo);
        fs::remove(marker);
    }
}
