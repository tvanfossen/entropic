// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh97_hybrid_cache.cpp
 * @brief gh#97: the prompt-cache / warm-keep prefix path must not corrupt KV
 *        bookkeeping on HYBRID (attention + recurrent/SSM) architectures.
 *
 * Regression from v2.7.5 (gh#96 warm-keep). On a hybrid arch (qwen35 / qwen35moe
 * / nemotron_h ∈ llm_arch_is_hybrid), warm-keep's partial `llama_memory_seq_rm(0, cut, -1)`
 * is REJECTED by the recurrent memory ("Mamba/RWKV can't have state partially
 * erased at the end"); the rejected return was discarded, so the decode of the
 * delta appended past the un-removed tail → KV position desync → "failed to
 * find a memory slot" with the cache mostly empty.
 *
 * BACKEND-DIRECT: drive a hybrid model (Nemotron-3-Nano-4B, arch nemotron_h —
 * Mamba2 + attention; v2.14.6: was the gh#97 repro model Qwen3.6-35B-A3B, arch
 * qwen35moe, which left the pared model set) through a
 * growing multi-turn conversation with the prompt cache + warm-keep ON. Every
 * turn must produce a clean, non-empty generation. On the broken rev a turn
 * fails (empty / error finish); once the cache prefix machinery is guarded off
 * for hybrid archs (fall back to full contiguous prefill), all turns succeed.
 *
 * This closes the arch-coverage gap: every gh#96 test ran on gemma4-E2B, a
 * plain llama_kv_cache where partial seq_rm behaves.
 *
 * Requires: GPU + Nemotron-3-Nano-4B GGUF. Run: ctest -L model -R gh97
 *
 * @version 2.7.6
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include "../../src/inference/llama_cpp_backend.h"

#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string filler(const std::string& tag) {
    return tag + ": the quick brown fox jumps over the lazy dog, and then "
           "the dog considers the fox carefully before responding in kind.";
}

}  // namespace

SCENARIO("gh#97: prompt-cache/warm-keep must not corrupt KV on a hybrid model",
         "[model][gh97]")
{
    GIVEN("a hybrid model (Nemotron-3-Nano-4B, arch nemotron_h)") {
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);
        fs::path gguf = fs::path(home) / ".entropic" / "models"
                        / "NVIDIA-Nemotron-3-Nano-4B-UD-Q4_K_XL.gguf";
        if (!fs::is_regular_file(gguf)) {
            SKIP("Nemotron-3-Nano-4B GGUF not present at " + gguf.string());
        }

        entropic::LlamaCppBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = gguf;
        cfg.adapter = "nemotron3";
        cfg.context_length = 8192;
        // 3.1 GB — fits the 11GB card whole. The 15-layer clamp was for the
        // 13GB Qwen3.6 IQ3_XXS; full offload loses nothing for this test.
        cfg.gpu_layers = 99;
        cfg.flash_attn = false;
        REQUIRE(backend.load(cfg));
        REQUIRE(backend.activate());
        // Prompt cache enabled + warm_keep on by default — the production path.

        std::vector<entropic::Message> msgs;
        msgs.push_back({"system",
            "You are a terse assistant. Answer in one short sentence."});

        entropic::GenerationParams params;
        params.max_tokens = 16;
        params.temperature = 0.0f;

        WHEN("running a growing multi-turn loop with warm-keep on") {
            constexpr int kTurns = 6;
            struct Turn { entropic::GenerationResult r; int input; int pos_max; };
            std::vector<Turn> turns;
            for (int t = 1; t <= kTurns; ++t) {
                msgs.push_back({"user", filler("turn " + std::to_string(t))});
                auto r = backend.generate(msgs, params);
                // Query BEFORE teardown — these are live on the active context.
                turns.push_back({r, backend.last_input_tokens(),
                                 backend.kv_pos_max()});
                msgs.push_back({"assistant",
                    r.content.empty() ? std::string("(none)") : r.content});
            }
            backend.deactivate();
            backend.unload();

            THEN("KV positions stay synced — no recurrent-memory desync") {
                for (size_t i = 0; i < turns.size(); ++i) {
                    INFO("turn " << (i + 1) << " input=" << turns[i].input
                         << " pos_max=" << turns[i].pos_max << " finish=["
                         << turns[i].r.finish_reason << "] content=["
                         << turns[i].r.content << "]");
                    CHECK(turns[i].r.error_code == 0);
                    CHECK_FALSE(turns[i].r.content.empty());
                    // Desync gate (deterministic): a correct prefill leaves
                    // pos_max ≈ input + generated - 1. The hybrid partial-seq_rm
                    // bug leaves an un-removed tail → pos_max inflates past
                    // input + max_tokens (RED by turn 2 on the buggy rev; with
                    // the hybrid guard / full prefill it stays bounded).
                    CHECK(turns[i].pos_max
                          < turns[i].input + params.max_tokens);
                }
            }
        }
    }
}
