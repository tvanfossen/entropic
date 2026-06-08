// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh98_batch.cpp
 * @brief gh#98: same-prefix batch generation prefills the shared prefix ONCE
 *        (the prefill-bound win) and stays correct vs serial; on a hybrid arch
 *        it falls back to serial (no seq-op corruption).
 *
 * BACKEND-DIRECT. The feature is an OPTIMIZATION, so a correctness-only test
 * passes even when batching never engages. Two INSTRUMENTATION assertions are
 * the genuine proof:
 *   1. last_gen_decode_calls() — the batched generation loop runs ONE decode
 *      per step over ALL N sequences, so this is ≈ the longest output length,
 *      NOT N·len. It is 0 unless the multi-seq path ran (a serial fallback
 *      never calls the batched loop). This is what distinguishes true multi-seq
 *      batching from both the serial path AND the already-shipped warm-keep
 *      (which shares the prefill but still decodes sequentially).
 *   2. last_prefill_tokens() — `shared + Σsuffix`, far below a per-request
 *      cold prefill × N (the shared prefix is prefilled once).
 *
 * Grammar `root ::= "GO" "GO" "GO"` forces a deterministic multi-token output,
 * so correctness is robust to GPU non-determinism and the gen loop runs >1 step.
 *
 * Requires: GPU + gemma4_e2b (+ Qwen3.6-35B-A3B for the hybrid case).
 * Run: ctest -L model -R gh98
 *
 * @version 2.8.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include "../../src/inference/llama_cpp_backend.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path model_path(const std::string& name) {
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / ".entropic" / "models" / name : fs::path{};
}

// N requests sharing a long verbatim system prompt, differing only in a short
// per-request user suffix — the gh#98 traffic shape.
std::vector<std::vector<entropic::Message>> same_prefix_requests(int n) {
    const std::string shared =
        "You are a terse game agent inside a large shared world. Follow the "
        "world constitution, respect the app context, and act in character. "
        "Emit exactly one action token and nothing else. Do not explain.";
    std::vector<std::vector<entropic::Message>> reqs;
    for (int i = 0; i < n; ++i) {
        entropic::Message sys;
        sys.role = "system";
        sys.content = shared;
        entropic::Message usr;
        usr.role = "user";
        usr.content = "Agent " + std::to_string(i) + ": act now.";
        reqs.push_back({sys, usr});
    }
    return reqs;
}

entropic::GenerationParams forced_grammar(const char* gbnf) {
    entropic::GenerationParams p;
    p.max_tokens = 4;
    p.temperature = 0.0f;
    p.grammar = gbnf;
    return p;
}

}  // namespace

SCENARIO("gh#98: multi-seq batch decodes N sequences together",
         "[model][gh98]")
{
    GIVEN("a plain-KV model (gemma4_e2b) with n_parallel >= the batch size") {
        fs::path gguf = model_path("gemma-4-E2B-it-Q8_0.gguf");
        if (!fs::is_regular_file(gguf)) {
            SKIP("gemma4_e2b GGUF not present at " + gguf.string());
        }

        constexpr int kN = 8;
        entropic::LlamaCppBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = gguf;
        cfg.adapter = "gemma4";
        cfg.context_length = 4096;
        cfg.gpu_layers = 99;
        cfg.flash_attn = false;
        cfg.n_parallel = kN;  // enough sequence slots for the batch
        REQUIRE(backend.load(cfg));
        REQUIRE(backend.activate());

        auto reqs = same_prefix_requests(kN);
        auto params = forced_grammar("root ::= \"GO\" \"GO\" \"GO\"\n");
        params.max_tokens = 16;

        WHEN("running one cold reference call, then the batch") {
            // Cold reference: one full prefill of a single request.
            backend.generate(reqs[0], params);
            int single_full = backend.last_prefill_tokens();

            std::vector<entropic::GenerationParams> pv(kN, params);
            std::atomic<bool> cancel{false};
            auto batch = backend.generate_batch(reqs, pv, cancel);
            int batch_prefill = backend.last_prefill_tokens();
            int gen_decodes = backend.last_gen_decode_calls();

            backend.deactivate();
            backend.unload();

            THEN("all N decode together, prefix prefilled once") {
                INFO("single_full=" << single_full
                     << " batch_prefill=" << batch_prefill
                     << " gen_decodes=" << gen_decodes);
                REQUIRE(batch.size() == static_cast<size_t>(kN));
                // Correctness: the grammar forces "GOGOGO" on every request.
                for (int i = 0; i < kN; ++i) {
                    CHECK(batch[i].content.find("GO") != std::string::npos);
                }
                // Multi-seq proof: the batched generation loop ran (a serial
                // fallback never calls it → 0) and did ONE decode per step over
                // ALL sequences, so the decode count is bounded by the output
                // length, NOT kN × it.
                CHECK(gen_decodes > 0);
                CHECK(gen_decodes <= params.max_tokens);
                // Prefix prefilled once: shared + Σsuffix, far below kN × a
                // single cold full prefill.
                CHECK(batch_prefill < single_full * (kN / 2));
            }
        }
    }
}

SCENARIO("gh#98: hybrid arch falls back to serial (no seq-op corruption)",
         "[model][gh98]")
{
    GIVEN("a hybrid model (Qwen3.6-35B-A3B, qwen35moe)") {
        fs::path gguf = model_path("Qwen3.6-35B-A3B-UD-IQ3_XXS.gguf");
        if (!fs::is_regular_file(gguf)) {
            SKIP("Qwen3.6-35B-A3B GGUF not present at " + gguf.string());
        }

        constexpr int kN = 4;
        entropic::LlamaCppBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = gguf;
        cfg.adapter = "qwen36";
        cfg.context_length = 8192;
        cfg.gpu_layers = 15;  // partial CPU offload — OOM-safe on 11GB
        cfg.flash_attn = false;
        cfg.n_parallel = kN;
        REQUIRE(backend.load(cfg));
        REQUIRE(backend.activate());

        auto reqs = same_prefix_requests(kN);
        std::vector<entropic::GenerationParams> pv(
            kN, forced_grammar("root ::= \"OK\"\n"));

        WHEN("running generate_batch on the hybrid arch") {
            std::atomic<bool> cancel{false};
            auto batch = backend.generate_batch(reqs, pv, cancel);
            // gh#98 instrumentation (audit task #71): run_batched_decode is the
            // ONLY writer of last_gen_decode_calls_; the hybrid guard routes to
            // the serial InferenceBackend::do_generate_batch, which never enters
            // that loop. 0 here proves the serial fallback was actually taken —
            // the inverse of the plain-KV sibling's CHECK(gen_decodes > 0).
            int gen_decodes = backend.last_gen_decode_calls();
            backend.deactivate();
            backend.unload();

            THEN("the serial fallback produces clean output for every request") {
                REQUIRE(batch.size() == static_cast<size_t>(kN));
                // Without this, a regression that ran the unsafe seq_cp/seq_rm
                // batched path on recurrent memory (gh#97) would still pass.
                INFO("gen_decodes=" << gen_decodes << " (must be 0 on fallback)");
                CHECK(gen_decodes == 0);
                for (int i = 0; i < kN; ++i) {
                    INFO("req " << i << " finish=[" << batch[i].finish_reason
                         << "] content=[" << batch[i].content << "]");
                    // is_hybrid_ guard → serial fallback (seq_cp/seq_rm on
                    // recurrent memory is unsafe — gh#97). Every request must
                    // still generate clean, grammar-forced output.
                    CHECK(batch[i].error_code == 0);
                    CHECK(batch[i].content.find("OK") != std::string::npos);
                }
            }
        }
    }
}
