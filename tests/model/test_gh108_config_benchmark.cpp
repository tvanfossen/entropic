// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh108_config_benchmark.cpp
 * @brief gh#108 (v2.9.3): side-by-side benchmark of the target optimized
 *        config vs the E4B Q8 baseline, same prompt, for human quality
 *        judgment + measured performance.
 *
 * Two configs, same prompt, greedy, run with thinking both ON and OFF:
 *   - "baseline": gemma-4-E4B-it-Q8_0.gguf, flash_attn on, f16 KV, plain decode.
 *   - "target":   gemma-4-E4B-it-qat-UD-Q2_K_XL.gguf (TQ2_0 mobile QAT) + MTP
 *                 head, flash_attn on, q4_0 KV, generate_mtp().
 *
 * This is NOT a correctness/regression gate — quantized-vs-Q8 output is
 * expected to differ (lossy weights), and quality is a human judgment call,
 * not an assertable property. Assertions are functional only (no error,
 * non-empty output) so this stays green in CI while printing the RAW,
 * unmodified output (with and without thinking) for a human to judge.
 * Both wall-clock (this file's own timer, includes any fixed per-call
 * overhead) and the backend's internal decode-only throughput are reported.
 */

#include "gh87_verify_helpers.h"  // LlamaCppBackend + config/result/message

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

// Shells out to nvidia-smi for actual device memory usage (MiB). Best-effort:
// returns -1 on any failure so callers can skip the reading rather than crash.
long query_vram_used_mb() {
    FILE* pipe = popen(
        "nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits -i 0", "r");
    if (!pipe) return -1;
    char buf[64] = {0};
    bool ok = std::fgets(buf, sizeof(buf), pipe) != nullptr;
    pclose(pipe);
    return ok ? std::strtol(buf, nullptr, 10) : -1;
}

entropic::ModelConfig base_cfg(const std::filesystem::path& path) {
    entropic::ModelConfig cfg;
    cfg.path = path;
    cfg.adapter = "gemma4";
    cfg.context_length = 4096;
    cfg.gpu_layers = 99;
    cfg.keep_warm = false;
    cfg.use_mlock = false;
    cfg.n_batch = 512;
    return cfg;
}

const char* kPrompt =
    "A farmer has 17 sheep. All but 9 die. How many are left? "
    "Explain your reasoning in one short sentence, then answer with just the number.";

struct RunStats {
    entropic::GenerationResult result;
    double wall_ms = 0.0;
};

// Runs one generate call (plain if head_path is empty, else generate_mtp)
// and times it with this file's own wall clock.
RunStats timed_run(entropic::LlamaCppBackend& backend,
                   const std::vector<entropic::Message>& conv,
                   const entropic::GenerationParams& params,
                   std::atomic<bool>& cancel,
                   const std::string& head_path,
                   int n_draft = 4) {
    RunStats stats;
    auto t0 = std::chrono::steady_clock::now();
    stats.result = head_path.empty()
        ? backend.generate(conv, params, cancel)
        : backend.generate_mtp(conv, params, {}, cancel, head_path, n_draft);
    auto t1 = std::chrono::steady_clock::now();
    stats.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return stats;
}

void print_run(const char* label, const RunStats& s, int max_tokens) {
    double wall_tok_s = (s.wall_ms > 0.0)
        ? 1000.0 * static_cast<double>(s.result.token_count) / s.wall_ms : 0.0;
    double accept_rate = (s.result.n_drafted > 0)
        ? static_cast<double>(s.result.n_accepted) / s.result.n_drafted : 0.0;
    std::printf(
        "----------------------------------------------------------------\n"
        "%s\n"
        "  max_tokens=%d tokens=%d wall_ms=%.1f wall_tok/s=%.2f "
        "decode_ms=%.1f decode_tok/s=%.2f finish=%s\n",
        label, max_tokens, s.result.token_count, s.wall_ms, wall_tok_s,
        s.result.generation_time_ms, s.result.throughput_tok_s,
        s.result.finish_reason.c_str());
    if (s.result.n_drafted > 0) {
        std::printf("  n_drafted=%d n_accepted=%d accept_rate=%.3f\n",
                   s.result.n_drafted, s.result.n_accepted, accept_rate);
    }
    std::printf("  RAW RESPONSE:\n%s\n", s.result.content.c_str());
}

}  // namespace

TEST_CASE("gh#108 benchmark: E4B Q8 baseline vs E4B Q2-mobile+MTP+flash+q4KV "
          "vs E4B Q4_K_XL+MTP+flash+q4KV",
          "[model][gh108][benchmark]") {
    auto q8_path = gh87verify::model_path("gemma-4-E4B-it-Q8_0.gguf");
    auto q2_path = gh87verify::model_path("gemma-4-E4B-it-qat-UD-Q2_K_XL.gguf");
    auto q4_path = gh87verify::model_path("gemma-4-E4B-it-UD-Q4_K_XL.gguf");
    auto head_path = gh87verify::model_path("mtp-gemma-4-E4B-it.gguf");
    if (!std::filesystem::is_regular_file(q8_path) ||
        !std::filesystem::is_regular_file(q2_path) ||
        !std::filesystem::is_regular_file(q4_path) ||
        !std::filesystem::is_regular_file(head_path)) {
        SKIP("baseline/target GGUF not present");
    }

    entropic::Message u;
    u.role = "user";
    u.content = kPrompt;
    std::vector<entropic::Message> conv{u};
    std::atomic<bool> cancel{false};
    // gh#108 follow-up: 400 truncated config [B] mid-thought (finish=length),
    // corrupting its throughput signal with an incomplete generation. Raised
    // so every config finishes naturally (finish=stop) for a fair, complete
    // decode-tok/s comparison — the "extended flow" needed to prove real
    // speedup rather than a short/noisy sample.
    const int kMaxTokens = 900;

    long vram_idle = query_vram_used_mb();
    std::printf(
        "\n================================================================\n"
        "gh#108 CONFIG BENCHMARK — same prompt, greedy, thinking ON and OFF\n"
        "PROMPT: %s\n"
        "GPU idle VRAM before any model loaded: %ld MiB\n", kPrompt, vram_idle);

    // --- Baseline: E4B Q8, flash on, f16 KV, plain decode ---
    entropic::ModelConfig q8_cfg = base_cfg(q8_path);
    q8_cfg.flash_attn = true;
    q8_cfg.cache_type_k = "f16";
    q8_cfg.cache_type_v = "f16";
    entropic::LlamaCppBackend q8_backend;
    REQUIRE(q8_backend.load(q8_cfg));
    REQUIRE(q8_backend.activate());
    long vram_a = query_vram_used_mb();

    entropic::GenerationParams p_think = {};
    p_think.max_tokens = kMaxTokens;
    p_think.temperature = 0.0f;
    p_think.enable_thinking = true;
    entropic::GenerationParams p_nothink = p_think;
    p_nothink.enable_thinking = false;

    auto q8_think = timed_run(q8_backend, conv, p_think, cancel, "");
    auto q8_nothink = timed_run(q8_backend, conv, p_nothink, cancel, "");
    q8_backend.deactivate();
    q8_backend.unload();

    std::printf("\n[A] BASELINE — E4B Q8_0, flash on, f16 KV, plain decode\n"
                "  VRAM: idle=%ld MiB -> active=%ld MiB (delta=%ld MiB)\n",
                vram_idle, vram_a, vram_a - vram_idle);
    print_run("[A] thinking=ON", q8_think, kMaxTokens);
    print_run("[A] thinking=OFF", q8_nothink, kMaxTokens);

    // --- Target: E4B Q2-mobile + MTP head, flash on, q4_0 KV ---
    entropic::ModelConfig q2_cfg = base_cfg(q2_path);
    q2_cfg.flash_attn = true;
    q2_cfg.cache_type_k = "q4_0";
    q2_cfg.cache_type_v = "q4_0";
    entropic::LlamaCppBackend q2_backend;
    REQUIRE(q2_backend.load(q2_cfg));
    REQUIRE(q2_backend.activate());
    long vram_b = query_vram_used_mb();

    // gh#108 Blackwell finding: n_draft optimum is trunk-dependent (Q2 -> 2,
    // Q8 -> 4). The engine's global default (config.h, tuned for Q8) is 4 —
    // wrong for this trunk. Use the documented Q2 optimum here.
    const int kQ2NDraft = 2;
    auto mtp_think = timed_run(q2_backend, conv, p_think, cancel, head_path.string(), kQ2NDraft);
    auto mtp_nothink = timed_run(q2_backend, conv, p_nothink, cancel, head_path.string(), kQ2NDraft);
    q2_backend.deactivate();
    q2_backend.unload();

    std::printf("\n[B] TARGET — E4B Q2-mobile (TQ2_0) + MTP, flash on, q4_0 KV\n"
                "  VRAM: idle=%ld MiB -> active=%ld MiB (delta=%ld MiB, vs [A]: %ld MiB)\n",
                vram_idle, vram_b, vram_b - vram_idle, vram_b - vram_a);
    print_run("[B] thinking=ON", mtp_think, kMaxTokens);
    print_run("[B] thinking=OFF", mtp_nothink, kMaxTokens);

    // --- Config C: E4B Q4_K_XL (no TQ2_0 tensors) + MTP head, flash on, q4_0 KV ---
    // gh#108 follow-up: the Q2-mobile trunk's 2 TQ2_0 tensors (tied
    // token_embd/output) have zero CUDA kernel support in this llama.cpp pin
    // (upstream #11183 stalled 18mo unmerged) and get forced onto CPU compute
    // every decode step. Q4_K_XL has no TQ2_0 tensors at all, so it should
    // avoid that CPU round-trip entirely while still getting flash+q4 KV+MTP.
    entropic::ModelConfig q4_cfg = base_cfg(q4_path);
    q4_cfg.flash_attn = true;
    q4_cfg.cache_type_k = "q4_0";
    q4_cfg.cache_type_v = "q4_0";
    entropic::LlamaCppBackend q4_backend;
    REQUIRE(q4_backend.load(q4_cfg));
    REQUIRE(q4_backend.activate());
    long vram_c = query_vram_used_mb();

    auto q4mtp_think = timed_run(q4_backend, conv, p_think, cancel, head_path.string(), 4);
    auto q4mtp_nothink = timed_run(q4_backend, conv, p_nothink, cancel, head_path.string(), 4);
    q4_backend.deactivate();
    q4_backend.unload();

    std::printf("\n[C] E4B Q4_K_XL + MTP, flash on, q4_0 KV (no TQ2_0 tensors)\n"
                "  VRAM: idle=%ld MiB -> active=%ld MiB (delta=%ld MiB, vs [A]: %ld MiB)\n",
                vram_idle, vram_c, vram_c - vram_idle, vram_c - vram_a);
    print_run("[C] thinking=ON", q4mtp_think, kMaxTokens);
    print_run("[C] thinking=OFF", q4mtp_nothink, kMaxTokens);

    double a_decode = q8_nothink.result.throughput_tok_s;
    double b_decode = mtp_nothink.result.throughput_tok_s;
    double c_decode = q4mtp_nothink.result.throughput_tok_s;
    std::printf(
        "================================================================\n"
        "decode-only speedup, thinking=OFF, vs [A] Q8 baseline:\n"
        "  [B] Q2-mobile+MTP+flash+q4KV = %.2fx\n"
        "  [C] Q4_K_XL+MTP+flash+q4KV   = %.2fx\n"
        "================================================================\n",
        (a_decode > 0.0) ? (b_decode / a_decode) : 0.0,
        (a_decode > 0.0) ? (c_decode / a_decode) : 0.0);

    REQUIRE(q8_think.result.error_code == 0);
    REQUIRE_FALSE(q8_think.result.content.empty());
    REQUIRE(q8_nothink.result.error_code == 0);
    REQUIRE_FALSE(q8_nothink.result.content.empty());
    REQUIRE(mtp_think.result.error_code == 0);
    REQUIRE_FALSE(mtp_think.result.content.empty());
    REQUIRE(q4mtp_think.result.error_code == 0);
    REQUIRE_FALSE(q4mtp_think.result.content.empty());
    REQUIRE(q4mtp_nothink.result.error_code == 0);
    REQUIRE_FALSE(q4mtp_nothink.result.content.empty());
    REQUIRE(mtp_nothink.result.error_code == 0);
    REQUIRE_FALSE(mtp_nothink.result.content.empty());
}
