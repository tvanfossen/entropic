// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh93_behavioral.cpp
 * @brief gh#93 behavioral model tests on the facade-driven harness (v2.8.0).
 *
 * These ride `facade_model_helpers.h`, which exercises the REAL
 * `configure_common` path (the seam the orchestrator-direct harness bypasses —
 * the gap that hid gh#88/90/94). Three behaviors:
 *   1. gh#94 — a tier with `enable_thinking:false` runs cleanly and emits no
 *      `<think>` block (the threaded sampler knob takes effect end-to-end).
 *   2. autoparser families (Qwen3.5) parse a MULTI-parameter tool call across a
 *      persistent-context session (the families that keep a hand-rolled adapter
 *      because common_chat's PEG autoparser grabs only the first parameter).
 *   3. cancel-bridge — `entropic_interrupt` from another thread stops a long
 *      run early (the run returns far faster than a full generation).
 *
 * Requires: GPU + gemma4_e2b (+ Qwen3.5-0.8B for #2). Run: ctest -L model -R gh93
 *
 * @version 2.8.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "facade_model_helpers.h"

namespace fs = std::filesystem;
using entropic::test::facade::FacadeProject;
using entropic::test::facade::model_gguf;
using entropic::test::facade::TierSpec;

namespace {

std::string run(entropic_handle_t h, const char* prompt) {
    char* out = nullptr;
    entropic_run(h, prompt, &out);
    std::string r = (out != nullptr) ? out : "";
    if (out != nullptr) { entropic_free(out); }
    return r;
}

}  // namespace

SCENARIO("gh#94: enable_thinking:false suppresses think blocks (facade)",
         "[model][gh93][gh94]")
{
    GIVEN("a gemma tier whose identity sets enable_thinking:false") {
        fs::path gguf = model_gguf("gemma-4-E2B-it-Q8_0.gguf");
        if (!fs::is_regular_file(gguf)) { SKIP("gemma4_e2b GGUF absent"); }

        FacadeProject proj("gh94_thinking");
        TierSpec t{"lead", "gemma4_e2b", "gemma4",
                   "You are a terse assistant. Answer in one short sentence."};
        t.context_length = 16384;
        t.enable_thinking = false;
        entropic_handle_t h = proj.setup({t});
        REQUIRE(h != nullptr);

        WHEN("a reasoning-flavored prompt is run") {
            std::string out = run(h, "What is 2+2? Think it through.");
            THEN("the output carries no <think> block and is non-empty") {
                INFO("out=[" << out << "]");
                CHECK(out.find("<think>") == std::string::npos);
                CHECK_FALSE(out.empty());
            }
        }
    }
}

SCENARIO("gh#93: autoparser family parses a multi-parameter tool call",
         "[model][gh93][autoparser]")
{
    GIVEN("a Qwen3.5 tier (hand-rolled multi-parameter adapter)") {
        fs::path gguf = model_gguf("Qwen3.5-0.8B-Q8_0.gguf");
        if (!fs::is_regular_file(gguf)) {
            SKIP("Qwen3.5-0.8B GGUF absent at " + gguf.string());
        }

        FacadeProject proj("gh93_autoparser");
        TierSpec t{"lead", "qwen3_5_0_8b", "qwen35",
                   "You are an agent. Use tools when asked."};
        t.context_length = 16384;
        entropic_handle_t h = proj.setup({t});
        REQUIRE(h != nullptr);

        WHEN("asked to call a 2-argument tool across two turns") {
            // Turn 1 primes the session; turn 2 asks for the multi-param call.
            run(h, "Acknowledge you can use the filesystem.write_file tool.");
            std::string out = run(
                h, "Call filesystem.write_file with path=/tmp/a.txt and "
                   "content=hello. Emit only the tool call.");
            THEN("BOTH parameters survive the parse (not just the first)") {
                INFO("out=[" << out << "]");
                // The autoparser bug drops everything after the first
                // <parameter=>; the hand-rolled adapter keeps both.
                CHECK(out.find("path") != std::string::npos);
                CHECK(out.find("content") != std::string::npos);
            }
        }
    }
}

SCENARIO("gh#93: entropic_interrupt stops a long run early (cancel-bridge)",
         "[model][gh93][cancel]")
{
    GIVEN("a gemma tier and a long generation request") {
        fs::path gguf = model_gguf("gemma-4-E2B-it-Q8_0.gguf");
        if (!fs::is_regular_file(gguf)) { SKIP("gemma4_e2b GGUF absent"); }

        FacadeProject proj("gh93_cancel");
        TierSpec t{"lead", "gemma4_e2b", "gemma4",
                   "You are a verbose storyteller."};
        t.context_length = 16384;
        entropic_handle_t h = proj.setup({t});
        REQUIRE(h != nullptr);

        WHEN("another thread interrupts shortly after the run starts") {
            using clock = std::chrono::steady_clock;
            std::thread interrupter([h]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                entropic_interrupt(h);
            });
            auto t0 = clock::now();
            std::string out = run(h, "Write an extremely long, detailed epic "
                                     "saga of at least two thousand words.");
            double secs = std::chrono::duration<double>(
                clock::now() - t0).count();
            interrupter.join();

            THEN("the run returns promptly (interrupt honored), not after a "
                 "full generation") {
                INFO("elapsed=" << secs << "s out.len=" << out.size());
                // A full multi-thousand-word generation would run many seconds;
                // the interrupt cuts it short. Generous bound to stay robust on
                // a busy box while still proving the interrupt was honored.
                CHECK(secs < 12.0);
            }
        }
    }
}
