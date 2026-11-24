// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v2146_host_robustness.cpp
 * @brief v2.14.6: three ways one turn took down a served host, through the
 *        real model and the real C API.
 *
 * - gh#214: a prompt that does not fit the tier's context. The typed
 *   refusal never left interface_factory.cpp (every wrapper returned 0), so
 *   the turn completed "successfully" with no content.
 * - gh#216: a tool result with a multi-byte character at the 200-byte cut
 *   poisoned the handle-wide tool history; every later turn threw.
 * - gh#217: release while another session decodes unloaded the model under
 *   the decode — a CUDA error that killed `entropic serve`.
 *
 * Requires: GPU, gemma-4-E4B QAT GGUF on disk.
 * Run: inv test --model --model-only --filter v2146
 *
 * @version 2.14.6
 */

#include "facade_model_helpers.h"
#include "model_test_context.h"
#include "engine_handle.h"  // white-box: engine->is_running()

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

CATCH_REGISTER_LISTENER(FacadeTestListener)

namespace {

namespace fs = std::filesystem;
using entropic::test::facade::FacadeProject;
using entropic::test::facade::TierSpec;

/// @brief The E2B QAT lead tier every scenario here uses.
TierSpec lead_tier() {
    TierSpec t;
    t.name = "lead";
    t.gguf_key = "gemma4_e4b_qat";
    t.adapter = "gemma4";
    t.identity_body = "You are a terse assistant.";
    t.context_length = 4096;
    t.explicit_completion = false;
    return t;
}

/// @brief Skip unless the model is on disk.
bool have_model() {
    auto gguf = entropic::test::facade::model_gguf("gemma-4-E4B-it-qat-UD-Q4_K_XL.gguf");
    return !gguf.empty() && fs::is_regular_file(gguf);
}

}  // namespace

SCENARIO("gh#214: a prompt that does not fit fails the turn, typed",
         "[model][v2146][gh214]") {
    if (!have_model()) { SKIP("gemma-4-E4B QAT GGUF not present"); }
    FacadeProject project("v2146_overflow");
    auto tier = lead_tier();
    tier.context_length = 2048;  // every tool staged (~5 K tokens) cannot fit
    tier.allowed_tools = {};
    auto* h = project.setup({tier});
    INFO("setup: " << project.setup_failure());
    REQUIRE(h != nullptr);

    char* out = nullptr;
    const auto rc = entropic_run_session(h, "s", "Say hello.", &out);
    const std::string why = entropic_last_error(h);
    entropic::test::facade::take_owned(out);
    INFO("rc=" << rc << " last_error=" << why);
    CHECK(rc == ENTROPIC_ERROR_TURN_FAILED);
    CHECK(why.find("context_overflow") != std::string::npos);
}

SCENARIO("gh#216: multi-byte text at the history cut does not poison later turns",
         "[model][v2146][gh216]") {
    if (!have_model()) { SKIP("gemma-4-E4B QAT GGUF not present"); }
    FacadeProject project("v2146_utf8_history");
    // Three files, offset by 0/1/2 ASCII bytes before a run of U+2014 (3
    // bytes each): whatever the tool's own framing adds, at least two of
    // the three put a split character at byte 200 of the result.
    const fs::path root = project.dir();
    std::string dashes;
    for (int i = 0; i < 200; ++i) { dashes += "\xE2\x80\x94"; }
    for (int pad = 0; pad < 3; ++pad) {
        std::ofstream(root / ("note" + std::to_string(pad) + ".txt"))
            << std::string(static_cast<size_t>(pad), 'a') << dashes << "\n";
    }
    auto tier = lead_tier();
    tier.allowed_tools = {"filesystem.read_file"};
    auto* h = project.setup({tier});
    INFO("setup: " << project.setup_failure());
    REQUIRE(h != nullptr);

    using entropic::test::facade::run_session_transcript;
    for (int pad = 0; pad < 3; ++pad) {
        run_session_transcript(h, "a", ("Use filesystem.read_file on note"
            + std::to_string(pad) + ".txt and tell me its first character.").c_str());
    }
    char* out = nullptr;
    const auto rc = entropic_run_session(h, "b", "What is 2 + 2? One word.", &out);
    entropic::test::facade::take_owned(out);
    INFO("rc=" << rc << " last_error=" << entropic_last_error(h));
    CHECK(rc == ENTROPIC_OK);  // pre-fix: GENERATE_FAILED, type_error.316
}

SCENARIO("gh#217: release is refused while another session is decoding",
         "[model][v2146][gh217]") {
    if (!have_model()) { SKIP("gemma-4-E4B QAT GGUF not present"); }
    FacadeProject project("v2146_release_mid_decode");
    auto tier = lead_tier();
    tier.allowed_tools = {"entropic.complete"};
    auto* h = project.setup({tier});
    INFO("setup: " << project.setup_failure());
    REQUIRE(h != nullptr);

    std::atomic<int> turn_rc{-1};
    std::thread decode([h, &turn_rc] {
        char* out = nullptr;
        turn_rc = entropic_run_session(
            h, "s", "Write the numbers from 1 to 400, one per line, "
                    "with nothing else.", &out);
        entropic_free(out);
    });
    // Wait until the turn is actually running, then let it get into decode.
    for (int i = 0; i < 600 && !h->engine->is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const bool mid_turn = h->engine->is_running();
    const auto release_rc = entropic_release_model(h, nullptr);  // pre-fix: CUDA abort
    decode.join();
    INFO("mid_turn=" << mid_turn << " release_rc=" << release_rc
         << " turn_rc=" << turn_rc.load());
    REQUIRE(mid_turn);  // engagement: the release really overlapped a decode
    CHECK(release_rc == ENTROPIC_ERROR_ALREADY_RUNNING);
    CHECK(turn_rc.load() == ENTROPIC_OK);
    // Once the turn is over, release works.
    CHECK(entropic_release_model(h, nullptr) == ENTROPIC_OK);
}
