// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh106_mtp_qat_mobile.cpp
 * @brief gh#106 (v2.9.0): MTP on the MOBILE QAT (TQ2_0 ternary) trunks (E2B+E4B).
 *
 * Both the mobile QAT quant (TQ2_0) and the MTP path are new in v2.9.0; their
 * INTERACTION is otherwise untested (the other MTP tests run the Q8 base trunk;
 * the QAT test runs standard-Q4 QAT without MTP). This validates each family's
 * MTP head loads against, and drives, its ternary-quantized trunk via shared KV.
 *
 * FUNCTIONAL only — NOT a performance gate. The ternary trunk's hidden states
 * are lower fidelity than the F16/Q8 the head was exported against, so the
 * accept-rate is low (the expected "no speedup on this quant" — measured ~0.12
 * for E2B vs ~0.26 on Q8). The assertions therefore gate FUNCTION, not benefit:
 *   - no error, coherent non-empty output, clean finish_reason
 *   - n_drafted > 0  → the head actually ran against the TQ2_0 trunk (the path
 *                      didn't crash or silently no-op). Accept-rate is reported
 *                      but NOT asserted.
 *
 * TQ2_0 is compute-bound on Pascal (~3x slower than Q4) — long ctest TIMEOUT.
 */

#include "gh87_verify_helpers.h"  // brings in LlamaCppBackend + config/result/message

#include <atomic>
#include <cstdio>

namespace {

// Run the functional MTP check on a (mobile-QAT trunk, MTP head) pair. SKIPs
// when either GGUF is absent. Returns nothing — asserts inline.
void run_mtp_qat_mobile(const char* trunk_file, const char* head_file,
                        const char* label) {
    auto target = gh87verify::model_path(trunk_file);
    auto head = gh87verify::model_path(head_file);
    if (!std::filesystem::is_regular_file(target)) {
        SKIP(std::string("mobile-QAT (TQ2_0) GGUF not present: ")
             + target.string());
    }
    if (!std::filesystem::is_regular_file(head)) {
        SKIP(std::string("MTP head GGUF not present: ") + head.string());
    }

    entropic::LlamaCppBackend backend;
    entropic::ModelConfig cfg;
    cfg.path = target;
    cfg.adapter = "gemma4";
    cfg.context_length = 4096;
    cfg.gpu_layers = 99;
    cfg.keep_warm = false;
    cfg.use_mlock = false;
    cfg.flash_attn = false;
    cfg.n_batch = 512;
    cfg.cache_type_k = "f16";
    cfg.cache_type_v = "f16";
    REQUIRE(backend.load(cfg));
    REQUIRE(backend.activate());

    entropic::Message u;
    u.role = "user";
    u.content = "Continue exactly, one number per line, up to 20:\n1\n2\n3\n4\n";
    entropic::GenerationParams params;
    params.max_tokens = 48;
    params.temperature = 0.0f;

    std::atomic<bool> cancel{false};
    std::function<void(std::string_view)> on_token;
    auto mtp = backend.generate_mtp({u}, params, on_token, cancel,
                                    head.string(), /*n_max=*/16);

    backend.deactivate();
    backend.unload();

    const float accept_rate = (mtp.n_drafted > 0)
        ? static_cast<float>(mtp.n_accepted) / static_cast<float>(mtp.n_drafted)
        : 0.0f;
    std::printf("\n===gh106 MTP × mobile-QAT(TQ2_0) [%s]===\n"
                "%d tok, drafted=%d accepted=%d accept_rate=%.3f finish=%s\n[%s]\n===\n",
                label, mtp.token_count, mtp.n_drafted, mtp.n_accepted,
                accept_rate, mtp.finish_reason.c_str(), mtp.content.c_str());
    INFO(label << " drafted=" << mtp.n_drafted << " accepted=" << mtp.n_accepted
         << " accept_rate=" << accept_rate);

    // FUNCTIONAL gates only (no perf/accept-rate floor — see file header).
    REQUIRE(mtp.error_code == 0);
    REQUIRE_FALSE(mtp.content.empty());
    REQUIRE((mtp.finish_reason == "stop" || mtp.finish_reason == "length"));
    // The head ran against the ternary trunk (no crash, no silent no-op).
    REQUIRE(mtp.n_drafted > 0);
}

}  // namespace

TEST_CASE("gh#106 MTP functions on the E2B mobile QAT (TQ2_0) trunk",
          "[model][gh106][mtp][qat][mobile]") {
    run_mtp_qat_mobile("gemma-4-E2B-it-qat-UD-Q2_K_XL.gguf",
                       "mtp-gemma-4-E2B-it.gguf", "E2B");
}

TEST_CASE("gh#106 MTP functions on the E4B mobile QAT (TQ2_0) trunk",
          "[model][gh106][mtp][qat][mobile]") {
    run_mtp_qat_mobile("gemma-4-E4B-it-qat-UD-Q2_K_XL.gguf",
                       "mtp-gemma-4-E4B-it.gguf", "E4B");
}
