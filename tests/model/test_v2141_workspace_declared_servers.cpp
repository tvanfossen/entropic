// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v2141_workspace_declared_servers.cpp
 * @brief v2.14.1: a server declared once in config is reachable from every
 *        workspace-bound session, and each session's call is answered by
 *        the instance running in ITS repository.
 *
 * The emergent multi-turn test decision #87 owed. One external stdio
 * server (the mock MCP server, advertising `read_project`, which returns
 * PROJECT.md from the child's own cwd) is declared once in
 * `mcp.external_servers`. Two workspaces on one resident model, two bound
 * sessions run concurrently. Through v2.14.0 a bound session was never
 * offered the tool at all; and even the base-set instance started in the
 * host's cwd (the stdio transport ignored its working_dir). Each answer
 * must carry its own repository's codename; neither transcript may carry
 * the other's.
 *
 * Multi-turn: the codename is asked for again after an unrelated turn, so
 * it comes from the conversation and a fresh call, not the last message.
 *
 * Requires: GPU, gemma-4-E2B QAT GGUF on disk, mock-mcp-server binary.
 * Run: inv test --model --model-only --filter v2141
 *
 * @version 2.14.1
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

/// @brief A repository whose PROJECT.md names a codename; no codename in its path.
fs::path make_repo(const std::string& slot, const std::string& codename) {
    auto dir = fs::temp_directory_path()
        / ("entropic_v2141_" + slot + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "PROJECT.md")
        << "This project's internal codename is " << codename << ".\n";
    return dir;
}

/// @brief One session's first and last answers plus its last transcript.
struct Turns {
    std::string first;
    std::string last;
    std::string transcript;
};

/// @brief Ask for the codename via the tool, change topic, ask again.
Turns run_turns(entropic_handle_t h, const std::string& key) {
    using entropic::test::facade::final_answer;
    using entropic::test::facade::run_session_transcript;
    Turns t;
    t.first = final_answer(run_session_transcript(h, key.c_str(),
        "Call the read_project tool, then tell me this project's codename "
        "in one word."));
    run_session_transcript(h, key.c_str(), "Name two fruits, nothing else.");
    t.transcript = run_session_transcript(h, key.c_str(),
        "Call read_project again and repeat the codename. One word.");
    t.last = final_answer(t.transcript);
    return t;
}

}  // namespace

SCENARIO("v2.14.1: a declared server serves each workspace from its own repository",
         "[model][v2141][workspace][external_servers]") {
    GIVEN("one resident model, one declared server, two repositories") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }
        const auto repo_a = make_repo("a", "Marigold");
        const auto repo_b = make_repo("b", "Saffron");

        entropic::test::facade::FacadeProject project("v2141_workspace_declared");
        project.extra_mcp_config =
            "  external_servers:\n"
            "    proj:\n"
            "      command: " MOCK_MCP_SERVER_PATH "\n"
            "      args: [\"--tools\", \"read_project\"]\n";
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body = "You are a terse assistant. Use the tools you are "
                             "given before answering about the project.";
        lead.context_length = 4096;
        lead.allowed_tools = {"proj.read_project"};
        lead.explicit_completion = false;
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        REQUIRE(entropic_workspace_create(h, "repo-a", repo_a.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_workspace_create(h, "repo-b", repo_b.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(h, "sess-a", "repo-a") == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(h, "sess-b", "repo-b") == ENTROPIC_OK);

        WHEN("both bound sessions ask at the same time") {
            Turns a;
            Turns b;
            std::thread ta([&] { a = run_turns(h, "sess-a"); });
            std::thread tb([&] { b = run_turns(h, "sess-b"); });
            ta.join();
            tb.join();
            INFO("A: [" << a.first << "] / [" << a.last << "]\nA transcript: " << a.transcript);
            INFO("B: [" << b.first << "] / [" << b.last << "]\nB transcript: " << b.transcript);

            THEN("each answers from its own repository's instance, both times") {
                CHECK(has_ci(a.first, "marigold"));
                CHECK(has_ci(a.last, "marigold"));
                CHECK(has_ci(b.first, "saffron"));
                CHECK(has_ci(b.last, "saffron"));
            }
            THEN("neither saw the other repository's file") {
                CHECK_FALSE(has_ci(a.transcript, "saffron"));
                CHECK_FALSE(has_ci(b.transcript, "marigold"));
            }
        }
        fs::remove_all(repo_a);
        fs::remove_all(repo_b);
    }
}
