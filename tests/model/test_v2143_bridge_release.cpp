// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v2143_bridge_release.cpp
 * @brief v2.14.3: `<prefix>.release` frees a loaded model over MCP, keeps
 *        the conversation, and the next ask reloads it.
 *
 * The emergent multi-turn test for the release tool. A session tells the
 * model a word (loading it), the bridge's `release` tool unloads it — the
 * residency snapshot must go empty, read from the engine rather than
 * inferred from the reply — and the SAME session then asks for the word:
 * the answer can only come from the conversation the release kept, decoded
 * by a model the ask reloaded.
 *
 * Requires: GPU, gemma-4-E2B QAT GGUF on disk.
 * Run: inv test --model --model-only --filter v2143
 *
 * @version 2.14.3
 */

#include "facade_model_helpers.h"
#include "model_test_context.h"
#include "engine_handle.h"  // white-box: external_bridge

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

CATCH_REGISTER_LISTENER(FacadeTestListener)

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

/// @brief Case-insensitive substring test.
bool has_ci(const std::string& hay, const std::string& needle) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

/// @brief How many models the engine reports resident.
size_t resident_count(entropic_handle_t h) {
    char* raw = nullptr;
    REQUIRE(entropic_residency_snapshot(h, &raw) == ENTROPIC_OK);
    const auto snap = json::parse(raw);
    entropic_free(raw);
    return snap.value("residency", json::array()).size();
}

/// @brief Call `entropic.release` through the bridge; return its text.
std::string call_release(entropic_handle_t h) {
    const json req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", {{"name", "entropic.release"}, {"arguments", json::object()}}}};
    const auto reply = json::parse(h->external_bridge->dispatch(req.dump(), -1));
    std::string text;
    for (const auto& c : reply["result"].value("content", json::array())) {
        text += c.value("text", "");
    }
    return text;
}

}  // namespace

SCENARIO("v2.14.3: release frees the model over MCP and the conversation survives",
         "[model][v2143][bridge][release]") {
    GIVEN("a served engine whose session has loaded the model") {
        auto gguf = entropic::test::facade::model_gguf("gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }
        entropic::test::facade::FacadeProject project("v2143_bridge_release");
        project.extra_mcp_config = "  external:\n    enabled: true\n";
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body = "You are a terse assistant with a good memory.";
        lead.context_length = 4096;
        lead.allowed_tools = {"entropic.complete"};
        lead.explicit_completion = false;
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);
        REQUIRE(h->external_bridge != nullptr);

        using entropic::test::facade::final_answer;
        using entropic::test::facade::run_session_transcript;
        run_session_transcript(h, "s", "Remember this word: Lighthouse. Reply with OK.");
        const size_t loaded = resident_count(h);

        WHEN("a client calls release, then the same session asks again") {
            const std::string released = call_release(h);
            const size_t after_release = resident_count(h);
            const std::string answer = final_answer(run_session_transcript(
                h, "s", "What word did I ask you to remember? One word."));
            const size_t after_ask = resident_count(h);
            INFO("release said: " << released << "\nanswer: " << answer);

            THEN("the model was resident, then released by name, then reloaded") {
                CHECK(loaded >= 1);
                CHECK(released.find("released: lead") != std::string::npos);
                CHECK(after_release == 0);
                CHECK(after_ask >= 1);
            }
            THEN("the conversation survived the release") {
                CHECK(has_ci(answer, "lighthouse"));
            }
        }
    }
}
