// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh110_mtp_agent_loop.cpp
 * @brief gh#110: MTP/speculative decoding must be reachable through the real
 *        agent loop (`entropic_run` → `AgentEngine::run_turn`), not just via a
 *        direct orchestrator call shaped like the agent loop.
 *
 * Prior coverage (test_gh106_mtp_route.cpp) drives `ctx.orchestrator->generate()`
 * directly — proving the kernel and its dispatch gate, but never proving the
 * facade actually reaches that gate. Two bugs made it unreachable in practice:
 *
 *  1. `build_loop_config()` (src/facade/entropic.cpp) hardcoded
 *     `lc.stream_output = true`. The streaming agent-loop path always binds a
 *     non-empty `on_token` callback, and `mtp_guard`'s envelope check
 *     (`mtp_unsupported_reason`, src/inference/mtp_envelope.h) rejects MTP
 *     whenever the callback is bound ("MTP does not support streaming") — so
 *     every agent-loop turn with `speculative.mtp` on failed loud, every time.
 *  2. Even with streaming disabled, `dispatch_batch_generate`
 *     (src/core/response_generator.cpp) preferred the cancel-aware
 *     `inference_.generate_cancellable` bridge, whose backing orchestrator
 *     call (`ModelOrchestrator::generate(..., cancel, tier)`) deliberately
 *     bypasses `run_generate_dispatch` (speculative routing) — batch-with-
 *     cancel only ever ran plain decode.
 *
 * Fix: `generation.stream_output` (config) threads into `LoopConfig`, and
 * `dispatch_batch_generate` prefers the non-cancellable dispatching
 * `inference_.generate` whenever speculative decoding is enabled.
 *
 * This test drives the full facade (`entropic_create` → `entropic_configure_dir`
 * → `entropic_run`) with `generation.stream_output: false` +
 * `inference.speculative.{enabled,mtp}: true` and asserts BOTH that the turn
 * produces real content and that the backend's own log line proves the
 * speculative kernel actually ran (n_drafted > 0 crosses the .so boundary only
 * as a log line — see llama_cpp_backend.cpp's "Speculative: generated=..."
 * emission, gated on n_drafted > 0).
 *
 * Requires: GPU + gemma4_e2b target GGUF + its MTP head GGUF. Run:
 * ctest -L model -R gh110
 *
 * @version 2.9.6
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>
#include <entropic/types/config.h>

#include "model_test_context.h"  // start_test_log/test_log_contains/end_test_log only

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

/// @brief Write a file, creating parent dirs.
/// @utility
/// @version 2.9.6
void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

}  // namespace

SCENARIO("gh#110: MTP engages through the real agent loop (entropic_run)",
         "[model][gh110][mtp][agent-loop]")
{
    GIVEN("a tier configured for gemma4 + MTP, batch delivery, speculative on") {
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);
        fs::path models_dir = fs::path(home) / ".entropic" / "models";
        fs::path target = models_dir / "gemma-4-E2B-it-Q8_0.gguf";
        fs::path head = models_dir / "mtp-gemma-4-E2B-it.gguf";
        if (!fs::is_regular_file(target) || !fs::is_regular_file(head)) {
            SKIP("MTP target/head GGUF not present under " + models_dir.string());
        }

        fs::path dir = fs::temp_directory_path() / "entropic_gh110_mtp_agent_loop";
        fs::remove_all(dir);
        fs::create_directories(dir);

        // gh#110: the two config keys the fix wires — stream_output=false so
        // the batch path is taken (streaming is unconditionally out of MTP's
        // envelope), and speculative.{enabled,mtp}=true routed through the
        // non-cancellable dispatching generate() so run_generate_dispatch
        // (and therefore MTP) is actually reached.
        write_file(dir / "config.local.yaml",
                   "models:\n"
                   "  lead:\n"
                   "    path: " + target.string() + "\n"
                   "    adapter: gemma4\n"
                   "    context_length: 4096\n"
                   "    gpu_layers: 99\n"
                   "    flash_attn: false\n"
                   "    cache_type_k: f16\n"
                   "    cache_type_v: f16\n"
                   "  default: lead\n"
                   "generation:\n"
                   "  stream_output: false\n"
                   "inference:\n"
                   "  speculative:\n"
                   "    enabled: true\n"
                   "    mtp: true\n"
                   "    n_draft: 16\n"
                   "    draft:\n"
                   "      path: " + head.string() + "\n"
                   "constitutional_validation:\n"
                   "  enabled: false\n");

        setenv("ENTROPIC_DATA_DIR",
               (fs::path(MODEL_PATH) / "data").string().c_str(), 1);

        start_test_log("gh110_mtp_agent_loop");

        entropic_handle_t h = nullptr;
        REQUIRE(entropic_create(&h) == ENTROPIC_OK);
        REQUIRE(entropic_configure_dir(h, dir.string().c_str()) == ENTROPIC_OK);

        WHEN("a real turn runs through entropic_run") {
            char* out = nullptr;
            auto rc = entropic_run(
                h, "Continue exactly, one number per line, up to 10:\n1\n2\n3\n",
                &out);
            std::string result = (out != nullptr) ? out : "";
            if (out != nullptr) { entropic_free(out); }
            entropic_destroy(h);

            THEN("the turn succeeds with real content AND MTP actually engaged") {
                INFO("rc=" << rc << "\nresult=[" << result << "]");
                REQUIRE(rc == ENTROPIC_OK);
                REQUIRE_FALSE(result.empty());
                // Decisive gate: this log line is only emitted by
                // LlamaCppBackend::generate_mtp when n_drafted > 0 — i.e. the
                // speculative kernel ran, not plain decode. Before the fix,
                // stream_output=true forced mtp_guard's streaming rejection
                // (turn would still "succeed" via plain decode fallback at the
                // engine layer, but this line would never appear); with
                // stream_output=false pre-fix, dispatch_batch_generate's
                // cancel-aware path bypassed run_generate_dispatch entirely,
                // so this line would also never appear.
                CHECK(test_log_contains("gh110_mtp_agent_loop",
                                        "Speculative: generated="));
                end_test_log();
            }
        }

        fs::remove_all(dir);
    }
}
