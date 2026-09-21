// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh166_workspaces.cpp
 * @brief gh#166: two repositories, one resident model, no file bleed.
 *
 * The mandated emergent multi-turn model test for a change that touches
 * tools and context. Two workspaces are created on ONE handle, two
 * sessions are bound one each, and both run CONCURRENTLY (the v2.13.0
 * default) through the public C ABI. Each repository holds a file at the
 * SAME relative path with different contents, so the load-bearing
 * assertion is the NEGATIVE one: neither session may report the other
 * repository's contents. A handle-wide tool root passes the positive
 * assertion by accident — both sessions would read the same file and
 * both would be "right".
 *
 * Multi-turn on purpose: the second turn asks about the file again after
 * unrelated turns, so the answer comes from accumulated context plus a
 * fresh tool call, not from the last message.
 *
 * @par Sizing (v2.13.0 fixture fix)
 * The scenario proves two workspaces do not read each other's files. That
 * is a property of tool-root resolution, not of model capability, so it is
 * run on the SMALLEST bundled GGUF that still tool-calls — gemma-4-E2B QAT
 * (~2.4 GiB). The first cut asked for gemma4_e4b (Q8_0, ~7.6 GiB of
 * weights), which the VRAM admission gate refused outright on the floor
 * hardware (a 1080 Ti, 11 GB) while the preceding model test's VRAM was
 * still coming back: footprint 8796933248 B vs budget 4520673280 B,
 * TIER_MODEL_TOO_LARGE, handle nullptr, and not one line of gh#166 ever
 * ran. The tier also names `allowed_tools`, because the default menu of 27
 * tools is ~5000 prompt tokens and would overflow the context on its own.
 *
 * Requires: GPU, gemma-4-E2B QAT GGUF on disk.
 * Run: ctest -L model -R gh166
 *
 * @version 2.13.0
 */

#include "facade_model_helpers.h"
#include "model_test_context.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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
 * @brief Create a repository holding one marker file.
 *
 * The directory name and the marker word are deliberately DECOUPLED.
 * The first cut named the directories after the markers
 * (`entropic_gh166_bazel_<pid>`), which made the load-bearing negative
 * assertion mean two different things at once: `has_ci(transcript,
 * "meson")` matched the other repository's CONTENT — what the scenario
 * claims — but equally the other repository's PATH, which proves
 * nothing. It matters now that the root confinement actually holds: a
 * refused cross-workspace read answers "Path escapes project root:
 * /tmp/...meson.../PROJECT.md", so under the old naming CONTAINMENT
 * WORKING would have FAILED the assertion. It also handed the model the
 * sibling root by analogy from its own — which is how the v2.13.0 gate
 * reached the hole in the first place.
 *
 * @param slot Opaque directory suffix ("a" / "b") — carries no marker.
 * @param marker Distinctive word, written ONLY inside the file.
 * @return Path to the created directory.
 * @utility
 * @version 2.13.0
 */
fs::path make_repo(const std::string& slot, const std::string& marker) {
    auto dir = fs::temp_directory_path() /
               ("entropic_gh166_" + slot + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "PROJECT.md")
        << "# Project\nThe build system for this project is "
        << marker << ".\n";
    return dir;
}

/**
 * @brief One bound session's last turn, both ways round.
 *
 * `entropic_run_session` returns the WHOLE serialized conversation, tool
 * results included. Asserting `has_ci(answer, "bazel")` on that string
 * matched the PROJECT.md contents the first turn had already read into the
 * history, so the positive half of this scenario could not fail — the same
 * defect gh#165 carried. The positive assertions now read `answer`; the
 * negative ones keep reading `transcript`, where a cross-workspace read
 * lands whether or not the model repeats it.
 * @version 2.13.0
 */
struct WorkspaceTurn {
    std::string answer;      ///< Final assistant message of the last turn
    std::string transcript;  ///< That turn's whole serialized conversation
};

/**
 * @brief Drive one bound session's multi-turn conversation.
 * @param h Engine handle.
 * @param key Session key (already bound to a workspace).
 * @return The last turn's answer and its transcript.
 * @utility
 * @version 2.13.0
 */
WorkspaceTurn run_workspace_turns(entropic_handle_t h,
                                  const std::string& key) {
    using entropic::test::facade::run_session_transcript;
    run_session_transcript(
        h, key.c_str(),
        "Read PROJECT.md in this project and tell me, in one short "
        "sentence, what the build system is.");
    run_session_transcript(h, key.c_str(),
                           "List two colours, nothing else.");
    WorkspaceTurn turn;
    turn.transcript = run_session_transcript(
        h, key.c_str(),
        "Name this project's build system again, exactly as PROJECT.md "
        "states it. One word.");
    turn.answer = entropic::test::facade::final_answer(turn.transcript);
    return turn;
}

}  // namespace

SCENARIO("gh#166: two workspaces on one handle do not read each other's "
         "files, concurrently", "[model][gh166][workspace]")
{
    GIVEN("one resident model and two repositories") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }

        auto repo_a = make_repo("a", "bazel");
        auto repo_b = make_repo("b", "meson");

        entropic::test::facade::FacadeProject project("gh166_workspaces");
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body =
            "You are a terse assistant with filesystem tools. Read files "
            "before answering about them. Answer in one short sentence.";
        lead.context_length = 4096;
        // Two reads are all this scenario needs. Without the allowlist the
        // handle stages every registered tool (~18.6 KB, ~5000 tokens) and
        // the 4 K context is spent before the task arrives.
        //
        // `entropic.complete` is the third because the tier OWES one. A
        // FacadeProject tier declares no auto_chain, so `populate_tier_info`
        // derives `explicit_completion: true`
        // (`.value_or(!tier.auto_chain.has_value())`,
        // src/facade/entropic.cpp:576) — the engine then requires every turn
        // to end in a tool call and nudges when one does not. With only the
        // two readers on the menu that nudge is unanswerable, and the v2.13.0
        // gate log shows exactly that: the same
        // "[SYSTEM] Your previous response contained no tool call. You must
        // end every turn with exactly one tool call
        // (filesystem.read_file,filesystem.list_directory). Retry." after
        // every turn, then "[iteration cap reached after 15 iterations]", and
        // a final answer of "I will read the `PROJECT.md` file again to
        // confirm the exact wording" instead of "bazel". Both positive
        // assertions failed on a model that was never allowed to finish. Same
        // defect and same one-tool fix as gh#165/gh#164 in 8008065.
        lead.allowed_tools = {"filesystem.read_file",
                              "filesystem.list_directory",
                              "entropic.complete"};
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        REQUIRE(entropic_workspace_create(h, "proj-a",
                                          repo_a.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_workspace_create(h, "proj-b",
                                          repo_b.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(h, "sess-a", "proj-a")
                == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(h, "sess-b", "proj-b")
                == ENTROPIC_OK);

        WHEN("both sessions run their turns at the same time") {
            WorkspaceTurn a;
            WorkspaceTurn b;
            std::thread ta([&] {
                a = run_workspace_turns(h, "sess-a");
            });
            std::thread tb([&] {
                b = run_workspace_turns(h, "sess-b");
            });
            ta.join();
            tb.join();

            THEN("each answers from ITS OWN repository") {
                // The ANSWER, not the transcript: the transcript already
                // holds the file the first turn read, so over it this
                // assertion passed however the model behaved.
                INFO("A answer: [" << a.answer << "]\nA transcript: "
                     << a.transcript);
                INFO("B answer: [" << b.answer << "]\nB transcript: "
                     << b.transcript);
                CHECK(has_ci(a.answer, "bazel"));
                CHECK(has_ci(b.answer, "meson"));
            }
            THEN("neither leaks the other repository's file") {
                // The assertion that fails on a handle-wide tool root:
                // both sessions would have read one PROJECT.md. Kept on the
                // TRANSCRIPT — a tool result lands in the conversation as a
                // user message, so a cross-workspace read is visible there
                // even when the model never mentions it.
                //
                // It fails on a SERVED cross-workspace read too, which is
                // what the v2.13.0 gate caught: the bundled config ships
                // `mcp.filesystem.allow_outside_root: true`, so until a
                // workspace forced its own confinement, an absolute path
                // into the sibling repository was read out and answered.
                // Since the markers no longer appear in any path, a match
                // here can only be the other repository's CONTENTS.
                INFO("A transcript: " << a.transcript);
                INFO("B transcript: " << b.transcript);
                CHECK_FALSE(has_ci(a.transcript, "meson"));
                CHECK_FALSE(has_ci(b.transcript, "bazel"));
            }
            THEN("neither repository was modified") {
                // No delegation isolation here; the point is that reads
                // resolved per workspace, and nothing wrote anywhere.
                std::ifstream in_a(repo_a / "PROJECT.md");
                std::string body((std::istreambuf_iterator<char>(in_a)),
                                 std::istreambuf_iterator<char>());
                CHECK(body.find("bazel") != std::string::npos);
            }
        }

        fs::remove_all(repo_a);
        fs::remove_all(repo_b);
    }
}
