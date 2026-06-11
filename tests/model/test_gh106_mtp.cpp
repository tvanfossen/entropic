// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh106_mtp.cpp
 * @brief gh#106 (v2.9.0): target-owned MTP speculative decode.
 *
 * The MTP head (mtp-gemma-4-E2B-it.gguf, a trunk-sharing "gemma4-assistant"
 * GGUF) drafts tokens the target verifies against its OWN greedy logits, so
 * the path is lossless BY CONSTRUCTION: at temp=0, sample_and_accept_n only
 * accepts a draft token when it equals the target's argmax.
 *
 * The dev GPU's greedy decode is non-deterministic run-to-run (see memory
 * gpu_nondeterministic_decode), so this CANNOT assert byte-identity vs plain
 * decode. Instead it gates on what a broken kernel would destroy:
 *   - ENGAGED      n_drafted > 0  → the head ran (not a silent plain-decode
 *                                   fallback). The TDD-red instrumentation
 *                                   assertion for an optimization.
 *   - ACCEPTED     n_accepted > 0 AND accept_rate above a floor → the drafts
 *                                   match the target's greedy path. A wrong
 *                                   verify-batch / pending_h seed / position
 *                                   would collapse acceptance to ~0.
 *   - COHERENT     non-empty content, clean finish_reason.
 *
 * Healthy acceptance on real text is the strongest GPU-tolerant proxy for
 * losslessness available.
 */

#include "gh87_verify_helpers.h"  // brings in LlamaCppBackend + config/result/message

#include <atomic>
#include <cstdio>

namespace {

entropic::ModelConfig mtp_target_cfg(const std::filesystem::path& path) {
    entropic::ModelConfig cfg;
    cfg.path = path;
    cfg.adapter = "gemma4";
    cfg.context_length = 4096;
    cfg.gpu_layers = 99;
    cfg.keep_warm = false;
    cfg.use_mlock = false;
    cfg.flash_attn = false;
    cfg.n_batch = 512;
    cfg.cache_type_k = "f16";
    cfg.cache_type_v = "f16";
    return cfg;
}

std::vector<entropic::Message> continuation_prompt() {
    entropic::Message u;
    u.role = "user";
    // A constrained continuation maximises the head's hit-rate — exactly the
    // regime where MTP should accept a healthy fraction of its drafts.
    u.content = "Continue this list exactly, one number per line, up to 20:\n"
                "1\n2\n3\n4\n";
    return {u};
}

}  // namespace

TEST_CASE("gh#106 MTP kernel: engaged + lossless-coherent",
          "[model][gh106][mtp][speculative]") {
    auto target = gh87verify::model_path("gemma-4-E2B-it-Q8_0.gguf");
    auto head = gh87verify::model_path("mtp-gemma-4-E2B-it.gguf");
    if (!std::filesystem::is_regular_file(target)) {
        SKIP("MTP target GGUF not present: " + target.string());
    }
    if (!std::filesystem::is_regular_file(head)) {
        SKIP("MTP head GGUF not present: " + head.string());
    }

    entropic::LlamaCppBackend backend;
    REQUIRE(backend.load(mtp_target_cfg(target)));
    REQUIRE(backend.activate());

    entropic::GenerationParams params;
    params.max_tokens = 64;
    params.temperature = 0.0f;  // greedy → lossless accept semantics

    // Plain baseline (same backend, same prompt) for an eyeball + structure ref.
    auto baseline = backend.generate(continuation_prompt(), params);

    std::atomic<bool> cancel{false};
    std::function<void(std::string_view)> on_token;  // empty: non-streaming
    auto mtp = backend.generate_mtp(continuation_prompt(), params, on_token,
                                    cancel, head.string(),
                                    /*n_max=*/16);

    backend.deactivate();
    backend.unload();

    const float accept_rate = (mtp.n_drafted > 0)
        ? static_cast<float>(mtp.n_accepted) / static_cast<float>(mtp.n_drafted)
        : 0.0f;
    std::printf("\n===gh106 MTP===\n"
                "baseline: %d tok, finish=%s\n[%s]\n"
                "mtp: %d tok, drafted=%d accepted=%d accept_rate=%.3f finish=%s\n[%s]\n"
                "===\n",
                baseline.token_count, baseline.finish_reason.c_str(),
                baseline.content.c_str(), mtp.token_count, mtp.n_drafted,
                mtp.n_accepted, accept_rate, mtp.finish_reason.c_str(),
                mtp.content.c_str());
    INFO("drafted=" << mtp.n_drafted << " accepted=" << mtp.n_accepted
         << " accept_rate=" << accept_rate);

    // No kernel error — the head loaded + the loop completed.
    REQUIRE(mtp.error_code == 0);
    // COHERENT: real output, clean termination.
    REQUIRE_FALSE(mtp.content.empty());
    REQUIRE((mtp.finish_reason == "stop" || mtp.finish_reason == "length"));
    // ENGAGED: the MTP head actually drafted (not a silent plain-decode
    // fallback). This is the instrumentation gate — it fails RED if routing
    // or setup quietly dropped MTP.
    REQUIRE(mtp.n_drafted > 0);
    // ACCEPTED: the target verified+accepted a healthy fraction of drafts —
    // a mis-seeded pending_h or mis-positioned verify batch would crater this.
    REQUIRE(mtp.n_accepted > 0);
    CHECK(accept_rate > 0.15f);
}
