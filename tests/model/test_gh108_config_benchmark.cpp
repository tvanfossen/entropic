// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh108_config_benchmark.cpp
 * @brief Decode-configuration benchmarks: the gh#108 quant/config comparison
 *        and the gh#153 four-arm MTP throughput measurement.
 *
 * Each case is its own ctest entry — and so its own process, which is what
 * returns VRAM between model loads (gh#142) — registered by tag in
 * tests/model/CMakeLists.txt and run through `inv bench`:
 *
 *   [quant-configs]  gh#108 (v2.9.3): E4B Q8 baseline vs E4B Q2-mobile+MTP vs
 *                    E4B Q4_K_XL+MTP, same prompt, thinking ON and OFF, for
 *                    human quality judgment. Functional assertions only.
 *   [mtp-e4b]        gh#153: MTP vs plain decode throughput on Gemma 4 E4B QAT,
 *                    fully offloaded — four arms, asserted against a
 *                    same-config floor.
 *   [mtp-a4b]        gh#153: the same comparison on Gemma 4 26B-A4B QAT,
 *                    partially offloaded with `gpu_layers: auto`.
 *   [mtp-a4b-experts] gh#153 #42(iii): the same comparison on the same A4B at
 *                    `gpu_layers: 31` (every layer) with `cpu_moe_layers: 31`
 *                    — the EXPERIMENTAL experts-to-host split (decision #75).
 *
 * The three gh#153 cases run the SAME code and are judged by DIFFERENT rules,
 * selected from the residency the engine resolved rather than from the case:
 * a fully resident trunk must show MTP clearing the floor, a partially
 * resident one must only show MTP not being materially worse than plain.
 * That is decision #74; the reasoning is restated at the residency block
 * below, next to the code that applies it.
 *
 * Residency is about WEIGHTS, not about layer indices. `cpu_moe_layers` puts a
 * layer's routed-expert FFN tensors on the host while its attention and KV stay
 * on the card, so `gpu_layers` can say 31 of 31 while most of the model's bytes
 * are in system RAM. A tier with `cpu_moe_layers > 0` is therefore NEVER
 * classified fully resident — see `fully_resident_for` below.
 *
 * A note on `throughput_tok_s`, which both halves read: the backend stamps its
 * clock BEFORE tokenize + prefill on the plain and the MTP path alike, so it is
 * tokens over the WHOLE generate call, not decode alone. The gh#153 cases print
 * each arm's prefill token count beside it so a cache-reuse asymmetry between
 * arms is visible rather than folded silently into the figure.
 */

#include "gh87_verify_helpers.h"  // LlamaCppBackend + config/result/message
#include "model_test_context.h"   // helpers only — NO CATCH_REGISTER_LISTENER

// src/inference is on this binary's include path (tests/model/CMakeLists.txt):
// the engine's own free-VRAM query, which the expert-offload engagement check
// measures against rather than inventing a second one.
#include "device_memory.h"

#include <entropic/config/bundled_models.h>
#include <entropic/config/loader.h>
#include <entropic/entropic.h>
#include <entropic/inference/orchestrator.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

// First line a shell command prints, newline stripped; "" on any failure.
// Best-effort by design: used only to RECORD the environment (VRAM, GPU name,
// git sha) next to a measurement, never to decide anything.
std::string shell_first_line(const std::string& cmd) {
    std::string line;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe != nullptr) {
        char buf[256] = {0};
        if (std::fgets(buf, sizeof(buf), pipe) != nullptr) { line = buf; }
        pclose(pipe);
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}

// Shells out to nvidia-smi for actual device memory usage (MiB). Best-effort:
// returns -1 on any failure so callers can skip the reading rather than crash.
long query_vram_used_mb() {
    const auto s = shell_first_line(
        "nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits -i 0 "
        "2>/dev/null");
    return s.empty() ? -1 : std::strtol(s.c_str(), nullptr, 10);
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
        "gen_ms=%.1f engine_tok/s=%.2f finish=%s\n",
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
          "[.][model][gh108][benchmark][quant-configs]") {
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
        "engine tok/s ratio (prefill included), thinking=OFF, vs [A] Q8 baseline:\n"
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

// ════════════════════════════════════════════════════════════════════════════
// gh#153 — MTP vs plain decode throughput, measured so it cannot be misread
// ════════════════════════════════════════════════════════════════════════════
//
// Decision #42 said MTP buys nothing on Pascal. That figure was taken at
// n_draft=16, a default #44 later called a net slowdown, and it was never under
// test: the suite asserted MTP ENGAGEMENT and never THROUGHPUT. A consumer's
// harness then produced five figures that were each withdrawn because the
// harness was measuring itself (decision #59), and the two that survived were
// later found to have run under a tier grammar that never resolved — valid only
// as "unconstrained" (#147, gh#154). The rules this case is built from, all of
// them from that history:
//
//   * Four arms, every run, same tier config, same prompt, same model load:
//       plain        plain decode
//       control      a SECOND plain arm, identical to `plain`
//       mtp          MTP head, n_draft=4
//       mtp_grammar  MTP under a TIER grammar named by bare stem — the exact
//                    spelling that failed open for the consumer
//     The control is not optional: |plain - control| is the noise floor, and an
//     effect is judged against it, never against zero.
//   * Tokens per second only. Output volume differs between arms (a grammar
//     changes what gets said), so a per-call duration is not a speed; none is
//     printed. The floor is printed ABOVE the effect.
//   * Figures come from the engine's own per-generation records —
//     ModelOrchestrator::generation_records(), the ring entropic_metrics_json
//     serializes as generations[] — never from log scraping or this file's
//     clock.
//   * An arm that did not do what its name says is INVALID and fails before
//     any figure is reported: MTP arms must show n_drafted > 0 on EVERY record,
//     plain arms n_drafted == 0, the grammar arm grammar.resolved with
//     source=tier and its output in the grammar's shape, the unconstrained arms
//     source=none.
//   * One discarded warm-up round (every arm once: cold prefill, MTP head
//     setup) before the measured rounds, which run in a Williams-balanced
//     order (kOrder) so neither floor arm inherits a fixed predecessor.
//   * No pinned multiplier. MTP must clear the floor by at least the floor
//     again, and by no less than kMinMarginPct.
//   * And no pinned RULE either: what MTP is required to do is chosen from
//     the residency the engine resolved, not from which case is running
//     (decision #74, `ResidencyRule` below).
//
// The tiers share one GGUF, so they share one backend (the orchestrator pools
// by path): every arm decodes on the same weights, the same KV configuration
// and the same resolved gpu_layers. They differ only in the per-tier
// `speculative.mtp` override and, for one arm, the tier `grammar:` stem.

namespace gh153 {

namespace fs = std::filesystem;

/// @brief Tier grammar stem the grammar arm names. A BARE stem, resolved from
///        `<config_dir>/grammars/` — the configuration that failed open in #147.
constexpr const char* kGrammarKey = "gh153_review";

/// @brief A code review as a JSON object. The prompt describes the same shape,
///        so every arm writes comparable content and the grammar arm differs
///        from `mtp` only by the constraint — which is what isolates the #147
///        question (does a grammar move the accept rate).
constexpr const char* kGrammarGbnf = R"GBNF(# gh#153 benchmark tier grammar: a code review as a JSON object.
root    ::= "{" ws "\"summary\":" ws string "," ws "\"findings\":" ws "[" ws finding ("," ws finding)* ws "]" ws "}"
finding ::= "{" ws "\"issue\":" ws string "," ws "\"fix\":" ws string ws "}"
string  ::= "\"" ([^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4}))* "\""
ws      ::= | " " | "\n" [ \t]{0,20}
)GBNF";

/// @brief The one prompt every arm answers. Long-form on purpose: a short
///        answer makes tok/s a measure of per-call overhead.
constexpr const char* kReviewPrompt =
    "Review the C function below. Answer with a JSON object that has two keys: "
    "\"summary\", a one-paragraph overall assessment, and \"findings\", an "
    "array with one object per problem, each with an \"issue\" key (what is "
    "wrong and why it matters) and a \"fix\" key (the corrected code for that "
    "problem). Report every bug, undefined behaviour, security problem and "
    "performance problem you can find.\n\n"
    "```c\n"
    "char *join_words(char **words, int n) {\n"
    "    char buf[64];\n"
    "    int i, len = 0;\n"
    "    for (i = 0; i <= n; i++) {\n"
    "        strcat(buf, words[i]);\n"
    "        strcat(buf, \" \");\n"
    "        len += strlen(words[i]);\n"
    "    }\n"
    "    char *out = malloc(len);\n"
    "    strcpy(out, buf);\n"
    "    return out;\n"
    "}\n"
    "```\n";

constexpr int kContextLength = 8192;
/// @brief The shipped default (config.h), and the value the consumer's
///        figures and decision #44 are stated at.
constexpr int kNDraft = 4;
/// @brief Least clearance above the floor, in percentage points, so a near-zero
///        floor from one lucky pair of runs cannot certify a trivial effect —
///        and, read the other way round, cannot condemn a trivial one either.
///        Both residency rules use it as their materiality threshold.
constexpr double kMinMarginPct = 5.0;

/**
 * @brief What MTP is required to do, chosen by how much of the trunk is
 *        actually on the GPU (decision #74).
 *
 * MTP's win tracks the RESIDENT FRACTION, not the card. Verifying a draft of
 * `n_draft` tokens is one forward pass over a batch of `n_draft + 1` rather
 * than of 1. On GPU-resident layers that batch is very nearly free: a decode
 * step there is bandwidth-bound on the weights, and the extra rows ride along
 * in the same read. On CPU-resident layers it is not — those layers are
 * compute-bound on a much slower unit, so verification cost scales with the
 * drafted batch, and on a MoE the drafted tokens can route to DIFFERENT
 * experts than the single token would have, multiplying the host-side work
 * again.
 *
 * Measured on one card with one engine at one n_draft (gh#153, GTX 1080 Ti,
 * n_draft=4): **+42.8 %** on a fully resident E4B QAT, **-0.92 %** on an A4B
 * QAT at 18 of 31 layers — and the A4B's accept rate was HIGHER (0.473 vs
 * 0.331), so the loss is not a drafting failure. A single pinned threshold
 * would have to be wrong for one of those two.
 *
 * The rule is therefore selected from what the engine RESOLVED:
 *
 *   FULLY RESIDENT      MTP must CLEAR the floor. The win is the claim.
 *   PARTIALLY RESIDENT  MTP must not be materially WORSE than plain. No win
 *                       is claimed; the assertion only holds the line that
 *                       enabling MTP does not COST anything under offload.
 *
 * Both outcomes are computed and printed on every run; only the one the
 * residency selects is asserted. The measured figure is always recorded WITH
 * the resident fraction, because it means nothing without it: the same A4B
 * on a 12 GB card is still partial, at a better fraction, and is expected to
 * report a BETTER number under the SAME rule rather than a different rule.
 *
 * The rule follows the WEIGHTS, so `cpu_moe_layers` is part of the input and
 * not a detail of the configuration: an experts-to-host tier has CPU-resident
 * weights in every layer, which is precisely the condition kNotWorse exists
 * for, however many layers `gpu_layers` claims. See `fully_resident_for`.
 */
enum class ResidencyRule {
    kClearsFloor,   ///< Fully resident: MTP must beat plain by > required_pct
    kNotWorse,      ///< Partial: MTP must be no worse than -allowed_loss_pct
};

/**
 * @brief Layers the engine actually placed on the GPU.
 *
 * Read from the config the BACKEND holds AFTER admission, where `auto` has
 * already become a number (decision #65) and `-1` still carries llama.cpp's
 * "every layer". The YAML string is never consulted: `-1` and `auto` both
 * state an intent, and only one of them survives contact with the card.
 *
 * llama.cpp's own `CPU_Mapped` buffer line would be the more direct
 * measurement, but it is stderr from vendored code, and this case takes every
 * figure from the engine's in-process records precisely so that nothing
 * depends on scraping a log. The layer count is the residency the engine
 * reports in process. Note the one thing it does not capture: at exactly
 * `gpu_layers == n_layer` llama.cpp leaves the non-repeating output tensors
 * host-side, so "fully resident" there means every REPEATING layer.
 *
 * @param gpu_layers Resolved offload; negative means all.
 * @param n_layer Model's repeating-layer count, or -1 when unreadable.
 * @return Layers on the GPU, clamped to [0, n_layer]; -1 when n_layer is.
 */
int resident_layers_for(int gpu_layers, int n_layer) {
    if (gpu_layers < 0) { return n_layer; }
    return std::min(gpu_layers, n_layer);
}

/**
 * @brief Is this trunk FULLY resident — every weight it has on the card?
 *
 * A layer count alone cannot answer that, and reading one as if it could is
 * the bug this function exists to close. `gpu_layers` is role-blind, but
 * `cpu_moe_layers` is not: it places the routed-expert FFN tensors of the
 * first N layers on the HOST while that layer's attention, KV, router,
 * shared/dense FFN and norms stay on the card (decision #75). A tier at
 * `gpu_layers: 31, cpu_moe_layers: 31` therefore reports 31 of 31 layers
 * resident while the large majority of a 26B-A4B's BYTES sit in system RAM —
 * and `judge()` would have asserted the STRICT, fully-resident MTP rule on
 * it, which is the exact misreading decision #74 was written to prevent.
 *
 * The rule is deliberately coarse: ANY expert offload disqualifies. This file
 * has no way to price how much moved — "the first N layers" is not "N layers'
 * worth of experts" (gemma4 decides per layer whether it has experts at all,
 * so patterns for dense blocks match nothing), and a resident FRACTION that
 * counted bytes would need GGUF tensor metadata the engine reads nowhere. A
 * coarse "not fully resident" is honest; an invented fraction would not be.
 *
 * @param resident_layers Layers `gpu_layers` placed on the GPU.
 * @param n_layer Model's repeating-layer count, or -1 when unreadable.
 * @param cpu_moe_layers Layers whose routed experts were sent to the host.
 * @return True only when every layer is on the card AND no expert left it.
 */
bool fully_resident_for(int resident_layers, int n_layer, int cpu_moe_layers) {
    return n_layer > 0 && resident_layers == n_layer && cpu_moe_layers == 0;
}

/**
 * @brief Where this tier's weights ended up, as one word for the log and JSON.
 *
 * The layer word first (`cpu` / `full` / `partial`), then `+experts_host` when
 * routed experts were moved off the card, so `full+experts_host` cannot be
 * skimmed as `full`: it says every LAYER was placed on the device and its
 * experts were not.
 *
 * @param gpu_layers Resolved offload count.
 * @param resident_layers Layers on the GPU.
 * @param n_layer Model's repeating-layer count.
 * @param cpu_moe_layers Layers whose routed experts went to the host.
 * @return One of cpu | full | partial, optionally with `+experts_host`.
 */
std::string offload_word(int gpu_layers, int resident_layers, int n_layer,
                         int cpu_moe_layers) {
    std::string word = "partial";
    if (gpu_layers == 0) {
        word = "cpu";
    } else if (n_layer > 0 && resident_layers == n_layer) {
        word = "full";
    }
    if (cpu_moe_layers > 0) { word += "+experts_host"; }
    return word;
}

/// @brief One arm: a tier name plus what that tier is configured to do.
struct ArmSpec {
    std::string name;  ///< Arm name == tier name
    bool mtp;          ///< Per-tier speculative.mtp override
    bool grammar;      ///< Tier names kGrammarKey
};

/// @brief The four arms. Index order is the one kOrder refers to.
const std::vector<ArmSpec>& arms() {
    static const std::vector<ArmSpec> k = {
        {"plain", false, false},
        {"control", false, false},
        {"mtp", true, false},
        {"mtp_grammar", true, true},
    };
    return k;
}

/// @brief Run order per round, as indices into arms(): a Williams square.
///        Over the four measured rounds every arm runs once in every position
///        AND follows every other arm exactly once. A plain rotation would
///        always put `mtp_grammar` before `plain` and `plain` before `control`,
///        so any carry-over from the previous generation (GPU clocks, the KV
///        it leaves resident) would land on the two floor arms unequally and
///        show up as noise that is not noise.
constexpr std::size_t kOrder[4][4] = {
    {0, 1, 3, 2},
    {1, 2, 0, 3},
    {2, 3, 1, 0},
    {3, 0, 2, 1},
};

/// @brief One model under test.
struct BenchModel {
    std::string label;       ///< e4b | a4b | a4b_experts — names the JSON
    std::string target_key;  ///< Registry key of the trunk
    std::string head_key;    ///< Registry key of the MTP head
    std::string gpu_layers;  ///< YAML value as written: "-1", "auto" or a count
    int max_tokens;          ///< Per-generation cap
    int measured_rounds;     ///< Rounds after the discarded warm-up (x4)

    /// @brief EXPERIMENTAL experts-to-host count; 0 leaves the key OUT of the
    ///        YAML entirely, so every pre-existing case's config is unchanged
    ///        byte for byte and their recorded `yaml` field still matches.
    int cpu_moe_layers = 0;

    /// @brief Physical micro-batch (`ModelConfig::n_ubatch`) for EVERY arm of
    ///        this case. Same omission rule as `cpu_moe_layers`: 0 leaves the
    ///        key out of the YAML entirely, so the three cases that do not set
    ///        it emit byte-identical tier blocks to before this field existed
    ///        and their recorded `yaml` still matches their measured figures.
    ///        Non-zero is how a case that does not fit its compute buffer at
    ///        the default 512 asks for a smaller one — see [mtp-a4b-iq2].
    int n_ubatch = 0;
};

/// @brief One generation, as the engine recorded it.
struct Trial {
    std::string arm;              ///< Arm / tier name
    int round = 0;                ///< 0 = discarded warm-up
    entropic::GenerationRecord rec;
};

/// @brief One arm's measured figures (warm-up excluded).
struct ArmStats {
    int trials = 0;
    int tokens = 0;
    double seconds = 0.0;    ///< Σ token_count / throughput_tok_s
    double tok_s = 0.0;      ///< tokens / seconds — token-weighted
    double tok_s_min = 0.0;  ///< Slowest single trial
    double tok_s_max = 0.0;  ///< Fastest single trial
    int n_drafted = 0;
    int n_accepted = 0;
    int prefill_tokens = 0;
    std::string grammar_source = "none";
    bool grammar_resolved = false;

    double accept_rate() const {
        return n_drafted > 0 ? static_cast<double>(n_accepted) / n_drafted
                             : 0.0;
    }
};

/// @brief Floor and effect, computed once and printed in that order, plus
///        BOTH residency rules' outcomes and which of them is asserted.
struct Verdict {
    double floor_pct = 0.0;     ///< |plain - control| / min(plain, control)
    double baseline = 0.0;      ///< The FASTER plain arm — conservative
    double effect_pct = 0.0;    ///< mtp vs baseline
    double margin_pct = 0.0;    ///< max(floor, kMinMarginPct) — materiality
    double required_pct = 0.0;  ///< kClearsFloor: effect must EXCEED this
    double allowed_loss_pct = 0.0;  ///< kNotWorse: effect must be >= -this
    bool clears = false;        ///< kClearsFloor's outcome, always computed
    bool not_worse = false;     ///< kNotWorse's outcome, always computed
    ResidencyRule rule = ResidencyRule::kClearsFloor;  ///< The ASSERTED one
    double resident_fraction = 0.0;  ///< Reported beside every figure
    bool passes = false;        ///< The asserted rule's outcome

    /// @brief One word for the rule, for the JSON summary and the log.
    const char* rule_id() const {
        return rule == ResidencyRule::kClearsFloor ? "clears_floor"
                                                   : "not_worse_than_plain";
    }
};

/// @brief What the run actually resolved to — recorded, not assumed.
struct RunInfo {
    std::string yaml;
    fs::path target_path;
    fs::path head_path;
    std::string quant;
    uint64_t target_bytes = 0;
    entropic::ModelConfig model;  ///< The backend's config AFTER admission
    int n_layer = -1;
    int resident_layers = -1;     ///< Of n_layer, after `auto`/`-1` resolved
    double resident_fraction = 0.0;  ///< resident_layers / n_layer
    /// @brief Layers whose routed experts the engine sent to the HOST. Read
    ///        back from the backend's config, not from what this file asked
    ///        for, and recorded beside `resident_fraction` everywhere that
    ///        fraction appears — the fraction is a LAYER count and says
    ///        nothing about the bytes this key moved off the card.
    int cpu_moe_layers = 0;
    bool fully_resident = false;  ///< Selects the rule — see ResidencyRule
    std::string offload;          ///< cpu|full|partial [+experts_host]
    long vram_used_mib = -1;      ///< Device-wide, after load (best-effort)
    long vram_used_mib_before = -1;  ///< Device-wide, before anything loaded
    long vram_free_mib_before = -1;  ///< ggml's own query, before the load
    std::string gpu;
    std::string git_sha;
};

/// @brief VRAM as it stood BEFORE this run loaded anything — the reference
///        the expert-offload engagement check is measured against.
struct PreLoadVram {
    long used_mib = -1;  ///< Device-wide (nvidia-smi); -1 when unreadable
    long free_mib = -1;  ///< What this load could take (ggml); -1 when no GPU
};

/**
 * @brief Sample both pre-load readings, nvidia-smi FIRST.
 *
 * The order is deliberate. `query_device_free_vram_bytes()` brings up a ggml
 * device, and with it a CUDA context worth a few hundred MiB. Taking the
 * nvidia-smi reading BEFORE that leaves the context inside the measured
 * after-minus-before delta, so the delta over-counts what the model load
 * itself took. That is the conservative direction for a check whose failure
 * mode is a knob that did nothing: it can only make the bound harder to clear.
 *
 * @return Both readings, each -1 when it could not be taken.
 */
PreLoadVram sample_pre_load_vram() {
    PreLoadVram v;
    v.used_mib = query_vram_used_mb();
    const uint64_t free_bytes = entropic::query_device_free_vram_bytes();
    v.free_mib = free_bytes > 0
        ? static_cast<long>(free_bytes / (1024ull * 1024ull)) : -1;
    return v;
}

/**
 * @brief Temp project directory whose grammars/ holds the tier grammar.
 *
 * `config_dir:` points here, so the orchestrator's own startup scan
 * (`load_bundled_grammars`) registers it — the production discovery path, not
 * a registration this file performs on the engine's behalf.
 */
class BenchProject {
public:
    explicit BenchProject(const std::string& label)
        : dir_(fs::temp_directory_path() / ("entropic_gh153_" + label)) {
        fs::remove_all(dir_);
        fs::create_directories(dir_ / "grammars");
        std::ofstream(dir_ / "grammars" / (std::string(kGrammarKey) + ".gbnf"))
            << kGrammarGbnf;
    }
    ~BenchProject() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    BenchProject(const BenchProject&) = delete;
    BenchProject& operator=(const BenchProject&) = delete;
    const fs::path& dir() const { return dir_; }

private:
    fs::path dir_;
};

/// @brief One tier block. Identical model config on every tier, by design.
std::string tier_yaml(const BenchModel& m, const ArmSpec& arm) {
    std::string y = "  " + arm.name + ":\n"
        "    path: " + m.target_key + "\n"
        "    adapter: gemma4\n"
        "    context_length: " + std::to_string(kContextLength) + "\n"
        "    gpu_layers: " + m.gpu_layers + "\n"
        "    flash_attn: true\n"
        "    cache_type_k: q4_0\n"
        "    cache_type_v: q4_0\n"
        "    use_mlock: false\n";
    // Every arm carries it, because the four tiers share one GGUF and the
    // orchestrator pools backends BY PATH: a tier that differed here would
    // silently decode on the other tier's placement.
    if (m.cpu_moe_layers > 0) {
        y += "    cpu_moe_layers: " + std::to_string(m.cpu_moe_layers) + "\n";
    }
    // Same reasoning, and the same all-arms-or-none rule: the head that
    // `speculative.mtp` builds inherits the TIER's cparams (build_mtp_head →
    // build_cparams(config())), so an arm that differed here would size its
    // draft context differently from the arm it is compared against.
    if (m.n_ubatch > 0) {
        y += "    n_ubatch: " + std::to_string(m.n_ubatch) + "\n";
    }
    y += "    speculative:\n"
         "      mtp: " + std::string(arm.mtp ? "true" : "false") + "\n";
    if (arm.grammar) {
        y += "    grammar: " + std::string(kGrammarKey) + "\n";
    }
    return y;
}

/// @brief The whole config, as YAML — parsed by the production loader.
std::string config_yaml(const BenchModel& m, const fs::path& dir) {
    std::string y = "config_dir: " + dir.string() + "\n"
        "models:\n"
        "  default: plain\n";
    for (const auto& arm : arms()) { y += tier_yaml(m, arm); }
    y += "routing:\n"
         "  enabled: false\n"
         "  fallback_tier: plain\n"
         "inference:\n"
         "  speculative:\n"
         "    enabled: true\n"
         "    mtp: true\n"
         "    n_draft: " + std::to_string(kNDraft) + "\n"
         "    draft:\n"
         "      path: " + m.head_key + "\n";
    return y;
}

/// @brief SKIP (with the reason) unless the GGUF is on disk.
void skip_if_absent(const std::string& key, const fs::path& path) {
    if (fs::is_regular_file(path)) { return; }
    entropic::test::SkipFacts f;
    f.key = key;
    f.path = path.string();
    SKIP(entropic::test::skip_reason_text(
        entropic::test::SkipCause::kGgufMissing, f));
}

/// @brief SKIP when an operator waived large-model runs on this host — the
///        same predicate both model-test load paths consult.
void skip_if_waived(const std::string& key, const fs::path& path,
                    uint64_t bytes) {
    if (!entropic::large_model_tests_waived(bytes)) { return; }
    entropic::test::SkipFacts f;
    f.key = key;
    f.path = path.string();
    f.file_bytes = bytes;
    SKIP(entropic::test::skip_reason_text(
        entropic::test::SkipCause::kLargeModelWaived, f));
}

/// @brief Parse the YAML through the production loader and bring the default
///        tier up. Fails loudly — a benchmark that cannot load has no result.
std::unique_ptr<entropic::ModelOrchestrator> build_orchestrator(
    const std::string& yaml, const entropic::config::BundledModels& registry) {
    entropic::ParsedConfig cfg;
    const auto err =
        entropic::config::load_config_from_string(yaml, registry, cfg);
    INFO("config rejected by the loader: " << err);
    REQUIRE(err.empty());
    auto orch = std::make_unique<entropic::ModelOrchestrator>();
    const bool ok = orch->initialize(cfg);
    INFO("orchestrator initialize failed; last residency error="
         << static_cast<int>(orch->last_residency_error()));
    REQUIRE(ok);
    return orch;
}

/// @brief Pre-flight: the tier grammar was discovered AND parses. The registry
///        keeps an invalid GBNF (flagged, not dropped) and `get()` still
///        returns its text, so "resolved" alone would not rule out a grammar
///        the sampler then fails to build.
void require_grammar_registered(entropic::ModelOrchestrator& orch) {
    const auto entries = orch.grammar_registry().list();
    const auto it = std::find_if(entries.begin(), entries.end(),
        [](const entropic::GrammarEntry& e) { return e.key == kGrammarKey; });
    {
        INFO("tier grammar '" << kGrammarKey << "' was not discovered under "
             "config_dir/grammars — the grammar arm could not be constrained");
        REQUIRE(it != entries.end());
    }
    INFO("tier grammar '" << kGrammarKey << "' failed GBNF validation: "
         << it->error);
    REQUIRE(it->validated);
}

/// @brief Record what the run resolved to: gpu_layers after `auto`, the real
///        layer count, and the environment.
RunInfo describe_run(const BenchModel& m,
                     const entropic::config::BundledModels& registry,
                     entropic::ModelOrchestrator& orch) {
    RunInfo info;
    info.target_path = registry.resolve(m.target_key);
    info.head_path = registry.resolve(m.head_key);
    info.quant = registry.get(m.target_key)->quant;
    std::error_code ec;
    info.target_bytes = fs::file_size(info.target_path, ec);
    auto* backend = orch.get_backend("plain");
    REQUIRE(backend != nullptr);
    info.model = backend->config();
    auto* llama = dynamic_cast<entropic::LlamaCppBackend*>(backend);
    if (llama != nullptr && llama->llama_model_ptr() != nullptr) {
        info.n_layer = llama_model_n_layer(llama->llama_model_ptr());
    }
    const int gl = info.model.gpu_layers;
    info.cpu_moe_layers = info.model.cpu_moe_layers;
    info.resident_layers = resident_layers_for(gl, info.n_layer);
    info.fully_resident = fully_resident_for(info.resident_layers, info.n_layer,
                                             info.cpu_moe_layers);
    info.resident_fraction = info.n_layer > 0
        ? static_cast<double>(info.resident_layers) / info.n_layer : 0.0;
    info.offload = offload_word(gl, info.resident_layers, info.n_layer,
                                info.cpu_moe_layers);
    info.vram_used_mib = query_vram_used_mb();
    info.gpu = shell_first_line(
        "nvidia-smi --query-gpu=name --format=csv,noheader -i 0 2>/dev/null");
    info.git_sha = shell_first_line(
        "git -C \"" + std::string(MODEL_PATH) + "\" rev-parse HEAD 2>/dev/null");
    return info;
}

// ────────────────────────────────────────────────────────────────────────────
// Did `cpu_moe_layers` actually DO anything?
// ────────────────────────────────────────────────────────────────────────────
//
// A prototype knob that silently did nothing must FAIL this case, never report
// a throughput number. llama.cpp offers no post-load per-tensor buffer query,
// so there is no direct "where did tensor X land". The strongest signal
// available in-process is device memory, and it is sufficient here only
// because the configuration is arranged to make it unambiguous:
//
//   MECHANISM. `gpu_layers` is asserted to have RESOLVED to the model's full
//   layer count. Every repeating layer was requested RESIDENT, so there is
//   nothing left in the engine that can put trunk weights on the host except
//   the expert override. That rules out both ways this arm could quietly
//   become an ordinary partial-offload run: the loader dropping the key, and
//   `auto` (which is refused in this combination anyway) resolving low.
//
//   EFFECT. A load with every layer resident and no override would hold
//   essentially the whole weights file on the card. The bound below says the
//   load must instead finish with real headroom left, and it is derived from
//   the two quantities this run measures for itself — the trunk's size on disk
//   and the VRAM this card had free — with no pinned number:
//
//       overflow = weights - free      bytes that CANNOT be on this card
//       bound    = free - overflow     the ceiling, cleared by the overflow again
//
//   "Cleared by the overflow again" is this file's own margin idiom, the same
//   shape as `required = floor + max(floor, 5 pp)`: the minimum is never the
//   margin. And here it is more than an idiom. `cpu_moe_layers` is not a
//   fitting algorithm — it has no idea how big the card is and moves EVERY
//   routed-expert tensor of the first N layers, not the least that would fit.
//   So an engaged override does not squeak under the ceiling, it lands far
//   below it; a load that merely squeaked under did not do what this knob does.
//
//   PRECONDITION. Both halves rest on the card being unable to hold the trunk.
//   On a card that CAN, a fully resident load fits, a low reading proves
//   nothing, and `bound` degenerates to a number nothing could exceed. That
//   case is SKIPPED with the reason, before the load, rather than passed.
//
// What this does NOT establish: how MUCH moved. "The first N layers" is not
// "N layers' worth of experts" — gemma4 decides per layer whether it has
// experts at all — and nothing here prices the split in bytes.

/// @brief The engagement check as numbers, all of them printed and recorded.
struct Engagement {
    long weights_mib = 0;    ///< The trunk's bytes on disk
    long free_mib = -1;      ///< VRAM available to this load, before it
    long overflow_mib = 0;   ///< weights - free: what cannot be on the card
    long bound_mib = 0;      ///< free - overflow: the ceiling to clear
    long used_mib = -1;      ///< Measured: VRAM this load consumed
    bool measurable = false; ///< Every reading came back
    bool engaged = false;    ///< used <= bound
};

/// @brief Bytes to MiB, the one conversion every figure here uses.
long to_mib(uint64_t bytes) {
    return static_cast<long>(bytes / (1024ull * 1024ull));
}

/**
 * @brief Compute the engagement figures from what this run measured.
 *
 * @param info The run as the engine resolved it, with both VRAM readings.
 * @return The figures; `measurable` false when a reading was unavailable.
 */
Engagement judge_engagement(const RunInfo& info) {
    Engagement e;
    e.weights_mib = to_mib(info.target_bytes);
    e.free_mib = info.vram_free_mib_before;
    if (info.vram_used_mib >= 0 && info.vram_used_mib_before >= 0) {
        e.used_mib = info.vram_used_mib - info.vram_used_mib_before;
    }
    e.measurable = e.weights_mib > 0 && e.free_mib > 0 && e.used_mib >= 0;
    if (e.measurable) {
        e.overflow_mib = e.weights_mib - e.free_mib;
        e.bound_mib = e.free_mib - e.overflow_mib;
        e.engaged = e.used_mib <= e.bound_mib;
    }
    return e;
}

/**
 * @brief Pre-flight for an expert-offload case, BEFORE the model is loaded.
 *
 * Fails loudly when there is no VRAM reading to be had, and SKIPs when this
 * card is large enough to hold the trunk — on such a card device memory
 * cannot distinguish an engaged override from a no-op, and a case that cannot
 * prove its knob did something has no business reporting a throughput figure.
 *
 * @param m The bench model, carrying `cpu_moe_layers` (0 = not this case).
 * @param target_bytes The trunk's size on disk.
 * @param pre Both pre-load VRAM readings.
 */
void require_measurable_expert_offload(const BenchModel& m,
                                       uint64_t target_bytes,
                                       const PreLoadVram& pre) {
    if (m.cpu_moe_layers <= 0) { return; }
    {
        INFO("cpu_moe_layers=" << m.cpu_moe_layers << " is EXPERIMENTAL and "
             "this case's only proof that it engaged is device memory, but "
             "nvidia-smi reported " << pre.used_mib << " MiB used and ggml "
             "reported " << pre.free_mib << " MiB free. Without both, a knob "
             "that did nothing would report a throughput number instead of "
             "failing — the one outcome this case may not produce.");
        REQUIRE(pre.used_mib >= 0);
        REQUIRE(pre.free_mib > 0);
    }
    const long weights_mib = to_mib(target_bytes);
    if (weights_mib > pre.free_mib) { return; }
    SKIP("expert-offload engagement cannot be proven on this card: the trunk "
         "is " + std::to_string(weights_mib) + " MiB and " +
         std::to_string(pre.free_mib) + " MiB of VRAM is free, so a load with "
         "every layer resident FITS and a low VRAM reading would not "
         "distinguish an engaged override from a no-op. This case measures "
         "the experts-only split on a card that cannot hold the model — the "
         "situation cpu_moe_layers exists for. Run it on one, or re-derive "
         "the assertion from something other than device memory.");
}

/// @brief Print the engagement figures, then assert them. Every number that
///        the verdict rests on is on screen before the verdict is.
void require_expert_offload_engaged(const BenchModel& m, const RunInfo& info,
                                    const Engagement& e) {
    if (m.cpu_moe_layers <= 0) { return; }
    std::printf(
        "----------------------------------------------------------------\n"
        "EXPERT OFFLOAD ENGAGEMENT — cpu_moe_layers=%d (requested %d)\n"
        "  trunk weights          %8ld MiB  on disk\n"
        "  VRAM free before load  %8ld MiB  (ggml, this device)\n"
        "  overflow               %8ld MiB  weights - free: bytes that CANNOT "
        "be on this card\n"
        "  bound                  %8ld MiB  free - overflow: the ceiling, "
        "cleared by the overflow again\n"
        "  VRAM this load took    %8ld MiB  (nvidia-smi device-wide, after - "
        "before)\n"
        "  gpu_layers resolved    %8d      of %d layers, every one requested "
        "RESIDENT\n"
        "  ENGAGED                %8s\n",
        info.cpu_moe_layers, m.cpu_moe_layers, e.weights_mib, e.free_mib,
        e.overflow_mib, e.bound_mib, e.used_mib, info.model.gpu_layers,
        info.n_layer, e.engaged ? "YES" : "NO");
    {
        INFO("the loader did not carry cpu_moe_layers to the backend: asked "
             "for " << m.cpu_moe_layers << ", the backend's post-admission "
             "config holds " << info.cpu_moe_layers);
        REQUIRE(info.cpu_moe_layers == m.cpu_moe_layers);
    }
    {
        INFO("a VRAM reading went missing between the pre-flight and the load "
             "(weights=" << e.weights_mib << " MiB, free=" << e.free_mib
             << " MiB, taken=" << e.used_mib << " MiB), so there is no "
             "engagement proof left and no figure may be reported");
        REQUIRE(e.measurable);
    }
    {
        INFO("gpu_layers resolved to " << info.model.gpu_layers << " of "
             << info.n_layer << " layers, so this is an ordinary PARTIAL "
             "offload and the VRAM reading cannot be attributed to the expert "
             "override. This case requires every layer to be REQUESTED "
             "resident, so that the override is the only thing left that can "
             "put trunk weights on the host.");
        REQUIRE(info.resident_layers == info.n_layer);
    }
    INFO("expert offload did NOT engage: the load took " << e.used_mib
         << " MiB of VRAM, above the " << e.bound_mib << " MiB bound derived "
         "from a " << e.weights_mib << " MiB trunk and " << e.free_mib
         << " MiB of free VRAM (overflow " << e.overflow_mib << " MiB, cleared "
         "again). With every layer requested resident, a load that takes that "
         "much device memory is holding the expert weights on the card.");
    REQUIRE(e.engaged);
}

/// @brief True when `text` has the grammar's shape: it opens `{"summary":`
///        and, when the decode stopped on its own, closes on `}`. A
///        length-capped decode is a grammar PREFIX and cannot be closed.
///        Unconstrained Gemma wraps JSON in a ```json fence, which fails both.
bool has_grammar_shape(const std::string& text, const std::string& finish) {
    const auto open = text.find_first_not_of(" \t\r\n");
    bool ok = open != std::string::npos && text[open] == '{';
    if (ok) {
        const auto key = text.find_first_not_of(" \t\r\n", open + 1);
        ok = key != std::string::npos
            && text.compare(key, 10, "\"summary\":") == 0;
    }
    if (ok && finish == "stop") {
        const auto close = text.find_last_not_of(" \t\r\n");
        ok = close != std::string::npos && text[close] == '}';
    }
    return ok;
}

/// @brief The grammar half of the integrity check.
void check_grammar(const ArmSpec& arm, const entropic::GenerationRecord& rec,
                   const entropic::GenerationResult& result,
                   const std::string& where, std::vector<std::string>& v) {
    const auto& g = rec.grammar;
    const std::string got = "source=" + g.source + " key='" + g.key
        + "' resolved=" + (g.resolved ? "true" : "false");
    if (arm.grammar) {
        if (!g.resolved || g.source != "tier" || g.key != kGrammarKey) {
            v.push_back(where + ": the tier grammar did not apply (" + got
                + "; expected source=tier key='" + kGrammarKey
                + "' resolved=true)");
        }
        const auto& text =
            result.raw_content.empty() ? result.content : result.raw_content;
        if (!has_grammar_shape(text, rec.finish_reason)) {
            v.push_back(where + ": output is not in the grammar's shape "
                "(finish=" + rec.finish_reason + "), so the constraint did "
                "not reach the sampler. Output:\n" + text);
        }
    } else if (g.resolved || g.source != "none") {
        v.push_back(where + ": an unconstrained arm was constrained (" + got
            + ")");
    }
    if (!g.conflict_winner.empty()) {
        v.push_back(where + ": two grammars collided, winner="
            + g.conflict_winner);
    }
}

/// @brief Every property an arm's name claims, checked on its record.
void check_record(const ArmSpec& arm, const entropic::GenerationRecord& rec,
                  const entropic::GenerationResult& result,
                  const std::string& where, std::vector<std::string>& v) {
    if (rec.token_count != result.token_count) {
        v.push_back(where + ": the newest record (" + std::to_string(
            rec.token_count) + " tokens) is not this generation ("
            + std::to_string(result.token_count) + " tokens)");
    }
    if (rec.token_count <= 0 || rec.throughput_tok_s <= 0.0) {
        v.push_back(where + ": nothing measurable was decoded (tokens="
            + std::to_string(rec.token_count) + ")");
    }
    if (arm.mtp && rec.n_drafted <= 0) {
        v.push_back(where + ": MTP did not engage (n_drafted=0) — this "
            "figure would be plain decode relabelled");
    }
    if (!arm.mtp && rec.n_drafted != 0) {
        v.push_back(where + ": a plain arm drafted "
            + std::to_string(rec.n_drafted) + " tokens — the floor would "
            "compare MTP against MTP");
    }
    check_grammar(arm, rec, result, where, v);
}

/// @brief Run one arm once and take its record from the engine's ring.
Trial run_trial(entropic::ModelOrchestrator& orch, const ArmSpec& arm,
                int round, const entropic::GenerationParams& params,
                std::vector<std::string>& violations) {
    entropic::Message u;
    u.role = "user";
    u.content = kReviewPrompt;
    const auto before = orch.generation_records().size();
    const auto result = orch.generate({u}, params, arm.name);
    const auto records = orch.generation_records();
    const std::string where = arm.name + " round " + std::to_string(round);

    Trial t;
    t.arm = arm.name;
    t.round = round;
    const bool appended = !records.empty()
        && (records.size() > before
            || before >= entropic::ModelOrchestrator::kMaxGenerationRecords);
    if (result.error_code != ENTROPIC_OK) {
        violations.push_back(where + ": generation failed, error "
            + std::to_string(static_cast<int>(result.error_code)) + ": "
            + result.error_message);
    } else if (!appended) {
        violations.push_back(where + ": no generation record was appended");
    } else {
        t.rec = records.back();
        check_record(arm, t.rec, result, where, violations);
    }
    // Progress only — no throughput here. A figure is reported once, after
    // every arm has been proven to be what it is called.
    std::printf("gh153 %-11s round %d%s: tokens=%d prefill=%d drafted=%d "
                "accepted=%d grammar=%s/%s finish=%s\n",
                arm.name.c_str(), round, round == 0 ? " (warm-up)" : "",
                t.rec.token_count, t.rec.prefill_tokens, t.rec.n_drafted,
                t.rec.n_accepted, t.rec.grammar.source.c_str(),
                t.rec.grammar.resolved ? "resolved" : "unresolved",
                t.rec.finish_reason.c_str());
    std::fflush(stdout);
    return t;
}

/// @brief Aggregate one arm's MEASURED trials (round 0 excluded).
ArmStats aggregate(const std::vector<Trial>& trials, const std::string& arm) {
    ArmStats s;
    for (const auto& t : trials) {
        if (t.arm != arm || t.round == 0) { continue; }
        const auto& r = t.rec;
        s.tokens += r.token_count;
        s.seconds += static_cast<double>(r.token_count) / r.throughput_tok_s;
        s.tok_s_min = s.trials == 0 ? r.throughput_tok_s
                                    : std::min(s.tok_s_min, r.throughput_tok_s);
        s.tok_s_max = std::max(s.tok_s_max, r.throughput_tok_s);
        s.n_drafted += r.n_drafted;
        s.n_accepted += r.n_accepted;
        s.prefill_tokens += r.prefill_tokens;
        s.grammar_source = r.grammar.source;
        s.grammar_resolved = r.grammar.resolved;
        ++s.trials;
    }
    s.tok_s = s.seconds > 0.0 ? s.tokens / s.seconds : 0.0;
    return s;
}

/// @brief Floor first, then the effect judged against it — under BOTH rules,
///        with the residency choosing which one is asserted.
///
/// The kNotWorse tolerance is the SAME quantity as the kClearsFloor margin,
/// `max(floor, kMinMarginPct)`, read the other way round. A floor that came
/// out near zero on one lucky pair of plain runs must not turn an ordinary
/// wobble into a recorded regression, exactly as it must not certify an
/// ordinary wobble as a win. On the A4B run that corrected decision #42 the
/// floor was 1.00 % and the effect -0.92 %: a bare-floor bound would have
/// passed that by 0.08 pp, which is not a margin, it is a coincidence.
Verdict judge(const ArmStats& plain, const ArmStats& control,
              const ArmStats& mtp, const RunInfo& info) {
    Verdict v;
    const double lo = std::min(plain.tok_s, control.tok_s);
    v.baseline = std::max(plain.tok_s, control.tok_s);
    v.floor_pct = lo > 0.0 ? 100.0 * (v.baseline - lo) / lo : 0.0;
    v.effect_pct = v.baseline > 0.0
        ? 100.0 * (mtp.tok_s - v.baseline) / v.baseline : 0.0;
    v.margin_pct = std::max(v.floor_pct, kMinMarginPct);
    v.required_pct = v.floor_pct + v.margin_pct;
    v.allowed_loss_pct = v.margin_pct;
    v.clears = v.effect_pct > v.required_pct;
    v.not_worse = v.effect_pct >= -v.allowed_loss_pct;
    v.rule = info.fully_resident ? ResidencyRule::kClearsFloor
                                 : ResidencyRule::kNotWorse;
    v.resident_fraction = info.resident_fraction;
    v.passes = info.fully_resident ? v.clears : v.not_worse;
    return v;
}

/// @brief One arm's headline line: tok/s with its token count beside it.
void print_arm(const std::string& name, const ArmStats& s) {
    std::printf("  %-12s %8.2f tok/s  %6d tokens  (per-trial %.2f .. %.2f)  "
                "prefill %d",
                name.c_str(), s.tok_s, s.tokens, s.tok_s_min, s.tok_s_max,
                s.prefill_tokens);
    if (s.n_drafted > 0) {
        std::printf("  accept %d/%d = %.3f", s.n_accepted, s.n_drafted,
                    s.accept_rate());
    }
    std::printf("\n");
}

/// @brief The configuration header — rule one of decision #59.
void print_config(const BenchModel& m, const RunInfo& info) {
    std::printf(
        "\n================================================================\n"
        "gh#153 MTP THROUGHPUT — %s (%s) + head %s\n"
        "  gpu_layers: %s -> %d (%s, %d layers)  cpu_moe_layers=%d  ctx=%d  "
        "flash_attn=%s  KV %s/%s  use_mlock=%s\n"
        "  n_draft=%d  temperature=0  max_tokens=%d  thinking=off  "
        "GPU %s, %ld MiB used device-wide after load\n"
        "  method: 1 discarded warm-up round (every arm once), then %d "
        "measured rounds in a Williams-balanced arm order\n"
        "  metric: tok/s = sum(tokens) / sum(tokens / throughput_tok_s) from "
        "the engine's generation records\n"
        "          (entropic_metrics_json generations[]); throughput_tok_s "
        "spans the whole generate call, prefill included\n",
        m.target_key.c_str(), info.quant.c_str(), m.head_key.c_str(),
        m.gpu_layers.c_str(), info.model.gpu_layers, info.offload.c_str(),
        info.n_layer, info.cpu_moe_layers, info.model.context_length,
        info.model.flash_attn ? "on" : "off",
        info.model.cache_type_k.c_str(), info.model.cache_type_v.c_str(),
        info.model.use_mlock ? "true" : "false", kNDraft, m.max_tokens,
        info.gpu.c_str(), info.vram_used_mib, m.measured_rounds);
}

/// @brief Inside the RESIDENCY block: where the EXPERTS went, when they went
///        anywhere. Without this the block reads "31 of 31 layers on the GPU"
///        and a reader has every reason to take that for a resident run.
void print_expert_note(const RunInfo& info) {
    if (info.cpu_moe_layers <= 0) { return; }
    std::printf(
        "  EXPERTS ARE HOST-SIDE — cpu_moe_layers=%d: the routed-expert FFN\n"
        "    tensors of the first %d layers are in SYSTEM RAM. The layer count\n"
        "    above is what `gpu_layers` placed, and `gpu_layers` is role-blind,\n"
        "    so %d of %d layers here is NOT a resident run — the large majority\n"
        "    of this model's BYTES are on the host, and only its attention, KV,\n"
        "    router, shared/dense FFN and norms are on the card. That is why\n"
        "    the partial rule is the one asserted (decisions #74, #75).\n",
        info.cpu_moe_layers, info.cpu_moe_layers, info.resident_layers,
        info.n_layer);
}

/// @brief The residency line and BOTH rules' outcomes. Printed BELOW the
///        floor and the effect, so neither can be read as the other; the
///        rule that is actually asserted is marked, so the one that is not
///        cannot be mistaken for the verdict.
void print_verdict(const RunInfo& info, const Verdict& v) {
    const bool full = v.rule == ResidencyRule::kClearsFloor;
    std::printf(
        "RESIDENCY — %d of %d layers on the GPU (%.1f %%, offload=%s, from "
        "the RESOLVED gpu_layers=%d)\n",
        info.resident_layers, info.n_layer, 100.0 * v.resident_fraction,
        info.offload.c_str(), info.model.gpu_layers);
    print_expert_note(info);
    std::printf("  rule: %s\n",
        full ? "FULLY RESIDENT — MTP must CLEAR the floor"
             : "PARTIALLY RESIDENT — MTP must only not be materially WORSE "
               "than plain; no win is claimed under partial offload");
    std::printf("  clears floor   %-3s  effect %+.2f %% > required %.2f %% "
                "(floor + max(floor, %.1f pp))%s\n",
                v.clears ? "YES" : "NO", v.effect_pct, v.required_pct,
                kMinMarginPct, full ? "   <- ASSERTED" : "");
    std::printf("  not worse      %-3s  effect %+.2f %% >= -%.2f %% "
                "(max(floor, %.1f pp))%s\n"
                "  VERDICT        %s (rule: %s, resident %.1f %%)\n",
                v.not_worse ? "YES" : "NO", v.effect_pct, v.allowed_loss_pct,
                kMinMarginPct, full ? "" : "   <- ASSERTED",
                v.passes ? "PASS" : "FAIL", v.rule_id(),
                100.0 * v.resident_fraction);
}

/// @brief The grammar arm, which is a #147 observation and not a claim.
void print_grammar_arm(const std::map<std::string, ArmStats>& s) {
    const auto& g = s.at("mtp_grammar");
    const auto& u = s.at("mtp");
    std::printf("MTP UNDER TIER GRAMMAR '%s' (source=%s, resolved=%s) — the "
                "#147 question\n", kGrammarKey, g.grammar_source.c_str(),
                g.grammar_resolved ? "true" : "false");
    print_arm("mtp_grammar", g);
    std::printf("  accept rate %.3f under the grammar vs %.3f unconstrained; "
                "tok/s %+.2f %% vs unconstrained mtp\n"
                "  NOT a speedup claim — this matrix has no plain arm under "
                "the grammar to compare against\n",
                g.accept_rate(), u.accept_rate(),
                u.tok_s > 0.0 ? 100.0 * (g.tok_s - u.tok_s) / u.tok_s : 0.0);
}

/// @brief Human report. The FLOOR is printed above the EFFECT so the two
///        cannot be read the other way round, and the RULE below both so it
///        reads as a judgment of them rather than as another measurement.
void print_report(const BenchModel& m, const RunInfo& info,
                  const std::map<std::string, ArmStats>& s, const Verdict& v) {
    print_config(m, info);
    std::printf("----------------------------------------------------------------\n"
                "NOISE FLOOR — two identical plain arms\n");
    print_arm("plain", s.at("plain"));
    print_arm("control", s.at("control"));
    std::printf("  floor        %8.2f %%\n"
                "EFFECT — MTP vs the faster plain arm\n", v.floor_pct);
    print_arm("mtp", s.at("mtp"));
    std::printf("  effect       %+8.2f %%\n", v.effect_pct);
    print_verdict(info, v);
    print_grammar_arm(s);
}

/// @brief Per-trial rows, printed only for a VALID run.
void print_trials(const std::vector<Trial>& trials) {
    std::printf("----------------------------------------------------------------\n"
                "per-trial records (round 0 = discarded warm-up)\n"
                "  %-12s round  tokens     tok/s  prefill  drafted  accepted  "
                "finish\n", "arm");
    for (const auto& t : trials) {
        std::printf("  %-12s %5d  %6d  %8.2f  %7d  %7d  %8d  %s\n",
                    t.arm.c_str(), t.round, t.rec.token_count,
                    t.rec.throughput_tok_s, t.rec.prefill_tokens,
                    t.rec.n_drafted, t.rec.n_accepted,
                    t.rec.finish_reason.c_str());
    }
}

/// @brief UTC timestamp for the summary.
std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

/// @brief Where the machine-readable summary lands.
fs::path summary_path(const std::string& label) {
    return fs::path(TEST_REPORTS_DIR).parent_path() / "bench"
         / ("gh153_mtp_" + label + ".json");
}

/// @brief The configuration block — every field read back from what the
///        engine resolved, not from what this file asked for. The residency
///        fields are the ones the rule is chosen from (decision #74), so
///        they sit next to the values they were derived from.
nlohmann::json config_json(const BenchModel& m, const RunInfo& info) {
    return {{"context_length", info.model.context_length},
            {"gpu_layers_configured", m.gpu_layers},
            {"gpu_layers_resolved", info.model.gpu_layers},
            {"n_layer", info.n_layer},
            {"resident_layers", info.resident_layers},
            {"resident_fraction", info.resident_fraction},
            // Schema /3: the resident FRACTION is a layer count. This is the
            // field that says whether those layers' weights were on the card.
            {"cpu_moe_layers", info.cpu_moe_layers},
            {"fully_resident", info.fully_resident},
            {"offload", info.offload},
            {"flash_attn", info.model.flash_attn},
            {"cache_type_k", info.model.cache_type_k},
            {"cache_type_v", info.model.cache_type_v},
            {"use_mlock", info.model.use_mlock},
            // Read back from the resolved config like every field around it.
            // `0` is llama.cpp's "match n_batch" and is what the three cases
            // that do not set the key record — a purely ADDITIVE key, so the
            // document stays /3: no existing field changes meaning.
            {"n_ubatch", info.model.n_ubatch},
            {"n_draft", kNDraft},
            {"temperature", 0.0},
            {"max_tokens", m.max_tokens},
            {"enable_thinking", false},
            {"vram_used_mib_before_load", info.vram_used_mib_before},
            {"vram_free_mib_before_load", info.vram_free_mib_before},
            {"vram_used_mib_after_load", info.vram_used_mib},
            {"yaml", info.yaml}};
}

/// @brief The engagement figures, so a reader of this file years from now can
///        check for themselves that the knob did something — and see that the
///        bound came from the trunk's size and this card, not from a constant.
nlohmann::json expert_offload_json(const RunInfo& info) {
    const auto e = judge_engagement(info);
    return {{"cpu_moe_layers", info.cpu_moe_layers},
            {"weights_mib", e.weights_mib},
            {"vram_free_mib_before_load", e.free_mib},
            {"overflow_mib", e.overflow_mib},
            {"bound_mib", e.bound_mib},
            {"vram_mib_taken_by_load", e.used_mib},
            {"measurable", e.measurable},
            {"engaged", e.engaged},
            {"bound", "free - (weights - free): with every layer REQUESTED "
                      "resident, nothing but the expert override can put trunk "
                      "weights on the host, so the load must finish clear of "
                      "the card's ceiling by the overflow again. llama.cpp "
                      "exposes no post-load per-tensor buffer query; this is "
                      "the strongest in-process signal available"},
            {"not_established", "HOW MUCH moved. 'The first N layers' is not "
                                "'N layers' worth of experts' — gemma4 decides "
                                "per layer whether it has experts — and "
                                "nothing here prices the split in bytes"}};
}

/// @brief The run header of the summary: what was measured, and how.
nlohmann::json run_json(const BenchModel& m, const RunInfo& info) {
    nlohmann::json doc = {
        // /3 (v2.13.0, gh#153 #42(iii)): `cpu_moe_layers` is recorded beside
        // every `resident_fraction` — in config, on every arm row and in
        // `effect` — and `fully_resident` now requires it to be 0, because a
        // layer whose experts are host-side is not a resident layer. `offload`
        // gains the `+experts_host` suffix and `expert_offload` carries the
        // engagement figures. /2 readers see only added keys, but a /2 reader
        // that infers residency from `resident_fraction` alone is wrong on a
        // /3 document and must read `cpu_moe_layers` too.
        {"schema", "entropic.gh153.mtp-bench/3"},
        {"issue", "gh#153"},
        {"entropic_version", entropic_version()},
        {"git_sha", info.git_sha},
        {"timestamp", utc_now()},
        {"gpu", info.gpu},
        {"model", {{"key", m.target_key},
                   {"file", info.target_path.filename().string()},
                   {"quant", info.quant},
                   {"file_bytes", info.target_bytes}}},
        {"mtp_head", {{"key", m.head_key},
                      {"file", info.head_path.filename().string()}}},
        {"config", config_json(m, info)},
        {"method", {{"warmup", "one discarded round, every arm once"},
                    {"measured_rounds", m.measured_rounds},
                    {"order", "Williams square: each arm once per position, "
                              "each arm after every other arm once"},
                    {"records", "ModelOrchestrator::generation_records() — "
                                "the ring entropic_metrics_json serializes as "
                                "generations[]"},
                    {"tok_s", "sum(token_count) / sum(token_count / "
                              "throughput_tok_s) over measured records"},
                    {"throughput_tok_s_spans", "the whole generate call, "
                                               "prefill included"},
                    {"floor", "|plain - control| / min(plain, control)"},
                    {"effect", "mtp / max(plain, control) - 1"},
                    {"required", "floor + max(floor, 5 pp)"},
                    {"rule", "decision #74 — the assertion is chosen from the "
                             "RESOLVED residency, not from the case: fully "
                             "resident, MTP must clear the floor; partially "
                             "resident, MTP must only be no worse than plain "
                             "by more than max(floor, 5 pp), because "
                             "verification cost scales with the drafted "
                             "batch on CPU-resident layers"},
                    {"residency", "fully resident means every layer on the "
                                  "card AND cpu_moe_layers == 0: an "
                                  "experts-to-host tier has CPU-resident "
                                  "weights in every layer however many layers "
                                  "gpu_layers claims, so it is judged by the "
                                  "partial rule"}}},
        {"prompt", kReviewPrompt},
        {"grammar", {{"key", kGrammarKey}, {"gbnf", kGrammarGbnf}}},
    };
    if (info.cpu_moe_layers > 0) {
        doc["expert_offload"] = expert_offload_json(info);
    }
    return doc;
}

/// @brief One arm's summary row — every field decision #42's rewrite needs.
nlohmann::json arm_json(const ArmSpec& arm, const ArmStats& s,
                        const BenchModel& m, const RunInfo& info) {
    return {
        {"arm", arm.name},
        {"model", m.target_key},
        {"quant", info.quant},
        {"gpu_layers", info.model.gpu_layers},
        {"offload", info.offload},
        // Beside the tok/s, not only in the config block: a row lifted out
        // of this array must carry the condition its figure depends on —
        // which is the layer fraction AND where those layers' experts went.
        {"resident_fraction", info.resident_fraction},
        {"cpu_moe_layers", info.cpu_moe_layers},
        {"mtp", arm.mtp},
        {"n_draft", arm.mtp ? kNDraft : 0},
        {"grammar", {{"source", s.grammar_source},
                     {"key", arm.grammar ? kGrammarKey : ""},
                     {"resolved", s.grammar_resolved}}},
        {"measured_trials", s.trials},
        {"tokens", s.tokens},
        {"tok_s", s.tok_s},
        {"tok_s_min", s.tok_s_min},
        {"tok_s_max", s.tok_s_max},
        {"n_drafted", s.n_drafted},
        {"n_accepted", s.n_accepted},
        {"accept_rate", arm.mtp ? nlohmann::json(s.accept_rate())
                                : nlohmann::json(nullptr)},
        {"prefill_tokens", s.prefill_tokens},
    };
}

/// @brief Every record, warm-up included and flagged.
nlohmann::json trials_json(const std::vector<Trial>& trials) {
    auto arr = nlohmann::json::array();
    for (const auto& t : trials) {
        arr.push_back({
            {"arm", t.arm},
            {"round", t.round},
            {"warmup", t.round == 0},
            {"finish_reason", t.rec.finish_reason},
            {"token_count", t.rec.token_count},
            {"prefill_tokens", t.rec.prefill_tokens},
            {"throughput_tok_s", t.rec.throughput_tok_s},
            {"n_drafted", t.rec.n_drafted},
            {"n_accepted", t.rec.n_accepted},
            {"grammar", {{"source", t.rec.grammar.source},
                         {"key", t.rec.grammar.key},
                         {"resolved", t.rec.grammar.resolved}}},
        });
    }
    return arr;
}

/// @brief Write the summary and say where it went.
void write_summary(const fs::path& path, const nlohmann::json& doc) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << doc.dump(2) << "\n";
    std::printf("summary JSON: %s\n", path.string().c_str());
}

/// @brief Summary for a VALID run: arms, floor, effect, grammar arm, trials.
nlohmann::json valid_summary(const BenchModel& m, const RunInfo& info,
                             const std::map<std::string, ArmStats>& s,
                             const Verdict& v,
                             const std::vector<Trial>& trials) {
    auto doc = run_json(m, info);
    doc["valid"] = true;
    auto rows = nlohmann::json::array();
    for (const auto& arm : arms()) {
        rows.push_back(arm_json(arm, s.at(arm.name), m, info));
    }
    doc["arms"] = rows;
    doc["floor"] = {{"plain_tok_s", s.at("plain").tok_s},
                    {"control_tok_s", s.at("control").tok_s},
                    {"floor_pct", v.floor_pct}};
    // Both rules' outcomes, the one that was ASSERTED, and the residency
    // that chose it — so a figure read out of this file years from now
    // cannot be detached from the condition that makes it mean anything.
    doc["effect"] = {{"mtp_tok_s", s.at("mtp").tok_s},
                     {"baseline_tok_s", v.baseline},
                     {"effect_pct", v.effect_pct},
                     {"floor_pct", v.floor_pct},
                     {"rule", v.rule_id()},
                     {"fully_resident", info.fully_resident},
                     {"resident_layers", info.resident_layers},
                     {"n_layer", info.n_layer},
                     {"resident_fraction", info.resident_fraction},
                     {"cpu_moe_layers", info.cpu_moe_layers},
                     {"required_pct", v.required_pct},
                     {"allowed_loss_pct", v.allowed_loss_pct},
                     {"clears_floor", v.clears},
                     {"not_worse_than_plain", v.not_worse},
                     {"passes", v.passes}};
    const auto& g = s.at("mtp_grammar");
    const auto& u = s.at("mtp");
    doc["grammar_arm"] = {
        {"accept_rate", g.accept_rate()},
        {"accept_rate_unconstrained_mtp", u.accept_rate()},
        {"tok_s_vs_unconstrained_mtp_pct",
         u.tok_s > 0.0 ? 100.0 * (g.tok_s - u.tok_s) / u.tok_s : 0.0},
        {"note", "no plain-under-grammar arm: not a speedup claim"}};
    doc["trials"] = trials_json(trials);
    return doc;
}

/// @brief Run all four arms on one model and judge MTP against the floor.
// ── gh#193: cold prefill against residency ───────────────────
//
// The four-arm bench above measures WARM DECODE over a short prompt. A
// consumer demonstrated from log timestamps that this is not the cost that
// dominates a real agentic turn: theirs carried a 7,879-character system
// prompt plus tool schemas plus history, each tier re-prefilling its own
// tail, and 516s of a 633s turn sat in three cold generations. Our
// `18.95 tok/s` cannot see any of it.
//
// It also matters for v2.13.1's own design claim. `gpu_layers: auto` prefers
// moving a MoE's experts host-side over dropping a layer, on the argument
// that attention is what prefill leans on. Nothing in this repository
// measured the prefill term, so that argument rested on decode figures that
// do not contain it.
//
// So: one COLD pass per residency level, `max_tokens=1`, over a deliberately
// large prompt. Wall time around that call is time-to-first-token — the
// figure a user actually waits on. A fresh orchestrator per level is what
// makes each pass cold; reusing one would measure the prompt cache.

/// @brief One residency level's cold-prefill measurement. @version 2.13.2
struct PrefillPoint {
    std::string gpu_layers;   ///< As written in YAML.
    int n_ubatch = 0;         ///< Physical batch this point used.
    int resolved_layers = 0;  ///< What the engine resolved it to.
    int cpu_moe_layers = 0;   ///< Experts held host-side.
    int prefill_tokens = 0;   ///< Reported by generations[] (gh#194).
    double ttft_ms = 0.0;     ///< Wall clock around a max_tokens=1 call.
    double prefill_tok_s = 0.0;
};

/**
 * @brief A prompt large enough that prefill dominates the call.
 * @param approx_tokens Rough token target.
 * @return Repeated prose, sized to approximate the target.
 * @utility
 * @version 2.13.2
 */
std::string large_prompt(int approx_tokens) {
    // ~1.3 tokens per word for English prose on this tokenizer family, so
    // aim by word count and report the token figure the engine measures
    // rather than trusting this estimate.
    static const char* kPara =
        "The depot inspection report records the culvert at mile marker "
        "twelve as structurally sound, with minor spalling on the north "
        "headwall and no observed scour at the outlet. Drainage capacity "
        "was measured at design flow and found adequate. ";
    const int words_per = 38;
    const int reps = std::max(1, approx_tokens * 10 / 13 / words_per);
    std::string out;
    out.reserve(static_cast<size_t>(reps) * std::strlen(kPara) + 64);
    for (int i = 0; i < reps; ++i) { out += kPara; }
    out += "\n\nIn one word: is the culvert sound?";
    return out;
}

/**
 * @brief Measure one cold prefill at one residency level.
 * @param m Bench model, with gpu_layers already set to the level.
 * @param registry Bundled model registry.
 * @param prompt The large prompt.
 * @return The measurement.
 * @utility
 * @req REQ-INFER-019
 * @version 2.13.2
 */
PrefillPoint measure_cold_prefill(const BenchModel& m,
                                  entropic::config::BundledModels& registry,
                                  const std::string& prompt) {
    PrefillPoint p;
    p.gpu_layers = m.gpu_layers;
    p.n_ubatch = m.n_ubatch;
    p.cpu_moe_layers = m.cpu_moe_layers;

    BenchProject project(m.label + "_" + m.gpu_layers);
    auto orch = build_orchestrator(config_yaml(m, project.dir()), registry);
    p.resolved_layers = describe_run(m, registry, *orch).resident_layers;

    entropic::Message u;
    u.role = "user";
    u.content = prompt;
    entropic::GenerationParams params;
    params.temperature = 0.0f;
    params.max_tokens = 1;  // prefill plus one token: this IS the TTFT call

    const auto t0 = std::chrono::steady_clock::now();
    const auto result = orch->generate({u}, params, "plain");
    const auto t1 = std::chrono::steady_clock::now();
    p.ttft_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    INFO("generation error: " << result.error_message);
    REQUIRE(result.error_code == ENTROPIC_OK);
    const auto records = orch->generation_records();
    REQUIRE_FALSE(records.empty());
    p.prefill_tokens = records.back().prefill_tokens;
    // gh#194 shipped in v2.13.2. If this is 0 the figure below is
    // meaningless, and silently reporting it would be worse than failing.
    REQUIRE(p.prefill_tokens > 0);
    if (p.ttft_ms > 0.0) {
        p.prefill_tok_s =
            static_cast<double>(p.prefill_tokens) / p.ttft_ms * 1000.0;
    }
    return p;
}

/**
 * @brief Sweep residency and report what cold prefill costs at each level.
 *
 * Reports prefill tokens per second, which is the quantity that moves with
 * placement. Deliberately makes no pass/fail claim about a THRESHOLD: this
 * establishes a figure the repository has never had, and one sample per
 * level cannot support a gate. What it does assert is that the measurement
 * happened — prefill was non-zero and the placement resolved as asked.
 *
 * @param base Bench model; its gpu_layers is replaced per level.
 * @param levels YAML gpu_layers values to sweep, most resident first.
 * @utility
 * @req REQ-INFER-019
 * @version 2.13.2
 */
void run_prefill_residency_bench(
    const BenchModel& base,
    const std::vector<std::pair<std::string, int>>& levels) {
    entropic::config::BundledModels registry;
    REQUIRE(load_registry(registry));
    REQUIRE(registry.get(base.target_key) != nullptr);
    skip_if_absent(base.target_key, registry.resolve(base.target_key));

    const std::string prompt = large_prompt(6000);
    std::vector<PrefillPoint> points;
    for (const auto& [layers, ubatch] : levels) {
        BenchModel m = base;
        m.gpu_layers = layers;
        m.n_ubatch = ubatch;
        points.push_back(measure_cold_prefill(m, registry, prompt));
    }

    std::printf("\ngh193 COLD PREFILL vs RESIDENCY — %s\n", base.label.c_str());
    std::printf("  one cold pass per level, max_tokens=1, prompt ~%d tokens\n",
                points.empty() ? 0 : points.front().prefill_tokens);
    std::printf("  %-10s %-9s %-9s %-12s %s\n",
                "gpu_layers", "resolved", "n_ubatch", "TTFT ms",
                "prefill tok/s");
    for (const auto& p : points) {
        std::printf("  %-10s %-9d %-9d %-12.1f %.1f\n",
                    p.gpu_layers.c_str(), p.resolved_layers, p.n_ubatch,
                    p.ttft_ms, p.prefill_tok_s);
    }
    if (points.size() >= 2) {
        const double best = points.front().prefill_tok_s;
        const double worst = points.back().prefill_tok_s;
        if (worst > 0.0) {
            std::printf("  most-resident is %.2fx the least-resident on "
                        "prefill\n", best / worst);
        }
    }
}

void run_four_arm_bench(const BenchModel& m) {
    entropic::config::BundledModels registry;
    REQUIRE(load_registry(registry));
    for (const auto* key : {&m.target_key, &m.head_key}) {
        INFO("registry key '" << *key << "' is missing from "
             "data/bundled_models.yaml");
        REQUIRE(registry.get(*key) != nullptr);
        skip_if_absent(*key, registry.resolve(*key));
    }
    std::error_code ec;
    const auto target_path = registry.resolve(m.target_key);
    const auto target_bytes = fs::file_size(target_path, ec);
    skip_if_waived(m.target_key, target_path, target_bytes);
    // A run that dies part-way must not leave the PREVIOUS run's figures
    // sitting under this name looking current.
    const auto out = summary_path(m.label);
    fs::remove(out, ec);
    wait_for_host_memory(static_cast<long>(target_bytes / (1024 * 1024)) + 2048);

    // Sampled before anything is loaded, so the after-minus-before delta is
    // this load's own device memory. It is also the reference the expert
    // offload engagement bound is derived from, which is why the case that
    // cannot be proven on this card is skipped HERE, before paying for a
    // multi-minute load that could not have told anyone anything.
    const auto pre = sample_pre_load_vram();
    require_measurable_expert_offload(m, target_bytes, pre);

    BenchProject project(m.label);
    const auto yaml = config_yaml(m, project.dir());
    std::printf("\ngh153 config (production loader):\n%s", yaml.c_str());
    auto orch = build_orchestrator(yaml, registry);
    require_grammar_registered(*orch);
    auto info = describe_run(m, registry, *orch);
    info.yaml = yaml;
    info.vram_used_mib_before = pre.used_mib;
    info.vram_free_mib_before = pre.free_mib;
    {
        INFO("gpu_layers=" << m.gpu_layers << " resolved to "
             << info.model.gpu_layers << " — nothing on the GPU, which is not "
             "the configuration this case measures");
        REQUIRE(info.model.gpu_layers != 0);
    }
    {
        // The rule is CHOSEN from the layer count (decision #74), so a layer
        // count we could not read is a broken run, not a case for guessing a
        // rule. Fail loud rather than fall back to the weaker assertion.
        INFO("the model's layer count could not be read, so the resident "
             "fraction is unknown and neither residency rule can be selected");
        REQUIRE(info.n_layer > 0);
    }
    // Before a single token is generated: an experimental knob that did
    // nothing must fail here, not produce a throughput figure that reads as a
    // measurement of a split that never happened. No-op for every other case.
    require_expert_offload_engaged(m, info, judge_engagement(info));

    entropic::GenerationParams params;
    params.temperature = 0.0f;
    params.max_tokens = m.max_tokens;
    params.enable_thinking = false;

    std::vector<Trial> trials;
    std::vector<std::string> violations;
    const auto& as = arms();
    // Round 0 is the warm-up and is never aggregated; measured round r runs
    // kOrder row (r - 1) % 4. Stops after the first round that produced a
    // violation: an invalid configuration is invalid in every round, and the
    // GPU time is better not spent proving it again.
    for (int round = 0; round <= m.measured_rounds && violations.empty();
         ++round) {
        const auto& row = kOrder[round == 0 ? 0 : (round - 1) % 4];
        for (const std::size_t idx : row) {
            trials.push_back(run_trial(*orch, as[idx], round, params, violations));
        }
    }

    if (!violations.empty()) {
        std::string all;
        for (const auto& v : violations) { all += "\n  - " + v; }
        std::printf("\ngh153 INVALID RUN — no figure is reported:%s\n",
                    all.c_str());
        auto doc = run_json(m, info);
        doc["valid"] = false;
        doc["violations"] = violations;
        write_summary(out, doc);
        INFO("INVALID: an arm did not do what its name says:" << all);
        REQUIRE(violations.empty());
    }

    std::map<std::string, ArmStats> stats;
    for (const auto& arm : as) { stats[arm.name] = aggregate(trials, arm.name); }
    const auto verdict =
        judge(stats.at("plain"), stats.at("control"), stats.at("mtp"), info);
    print_report(m, info, stats, verdict);
    print_trials(trials);
    write_summary(out, valid_summary(m, info, stats, verdict, trials));
    std::printf("================================================================\n");

    INFO("MTP " << stats.at("mtp").tok_s << " tok/s vs the faster plain arm "
         << verdict.baseline << " tok/s: effect " << verdict.effect_pct
         << " % against a floor of " << verdict.floor_pct << " %, with "
         << info.resident_layers << " of " << info.n_layer
         << " layers resident (" << 100.0 * info.resident_fraction
         << " %), judged by '" << verdict.rule_id() << "' — "
         << (info.fully_resident
                 ? "fully resident, so the effect must EXCEED "
                 : "partially resident, so the effect must be no worse than -")
         << (info.fully_resident ? verdict.required_pct
                                 : verdict.allowed_loss_pct)
         << " %");
    REQUIRE(verdict.passes);
}

}  // namespace gh153

TEST_CASE("gh#153 MTP vs plain decode throughput — four arms, Gemma 4 E4B "
          "QAT fully offloaded",
          "[.][model][gh153][benchmark][mtp-e4b]") {
    // The consumer's configuration (#153): E4B QAT + its head, flash, q4_0 KV,
    // every layer on the GPU. `-1` is an INTENT — the case asserts the
    // fully-resident rule only because 42 of 42 layers came back resident.
    //
    // Measured 2026-09 on a GTX 1080 Ti: plain 53.11 / control 53.09 / mtp
    // 75.84 tok/s (accept 0.331), floor 0.045 %, effect +42.8 % (decision
    // #42). That is what CLEARS THE FLOOR is expected to print here.
    gh153::run_four_arm_bench(
        {"e4b", "gemma4_e4b_qat", "mtp_e4b", "-1", 512, 4});
}

TEST_CASE("gh#153 MTP vs plain decode throughput — four arms, Gemma 4 26B-A4B "
          "QAT partially offloaded (gpu_layers auto)",
          "[.][model][gh153][benchmark][mtp-a4b]") {
    // 14.25 GB on the 11 GB floor card: `gpu_layers: auto` (decision #65)
    // derives the split from free VRAM and the summary records what it chose.
    // Shorter generations than E4B — a partially offloaded MoE decodes several
    // times slower — so the four arms still fit a bounded run.
    //
    // This case does NOT assert a speedup, and the reason is measured rather
    // than assumed (decision #74). On the same 1080 Ti at 18 of 31 layers:
    // plain 18.86 / control 18.68 / mtp 18.68 tok/s, floor 1.00 %, effect
    // -0.92 % — with an accept rate of 0.473, HIGHER than the fully resident
    // E4B's 0.331. Drafting works; the verify is what costs, on the 13 layers
    // that are not on the card. The MTP head itself was fully resident (5/5
    // layers offloaded), so a CPU-side drafter is not the explanation.
    //
    // The same rule is expected to hold on a 12 GB card, where this model is
    // still partial at a better fraction: a better number, under this rule,
    // not a different rule.
    gh153::run_four_arm_bench(
        {"a4b", "gemma4_a4b_qat", "mtp_a4b", "auto", 256, 4});
}

TEST_CASE("gh#153 MTP vs plain decode throughput — four arms, Gemma 4 26B-A4B "
          "QAT with every layer placed and the routed experts on the host",
          "[.][model][gh153][benchmark][mtp-a4b-experts]") {
    // The same model, card, prompt, arms, floor and rules as [mtp-a4b]. One
    // thing changes: `cpu_moe_layers` sends the routed-expert FFN tensors of
    // all 31 layers to the host so attention, its KV, the router, the
    // shared/dense FFN and the norms can stay on the card (decision #75).
    // That is the split #42(iii) says its figures do not cover, and this case
    // exists so the comparison against #42(b)'s 18-of-31 run can be made.
    //
    // `gpu_layers: 31` is an EXPLICIT COUNT, and the three ways of writing
    // "all of them" are each wrong here:
    //   -1 / 99  classify as `Offload::full` in vram_footprint.h, so the gh#148
    //            admission gate prices the whole 14.25 GB file against an
    //            11 GiB card and refuses the tier before the knob can help.
    //   auto     is refused by design in this combination: the residency math
    //            prices a layer as a WHOLE layer and has no model of expert
    //            placement, so the split it derives would not describe the
    //            load (expert_offload_conflict_reason).
    // 31 classifies as `partial_unknown`, which leaves the gate open — the
    // engine declines to price what it cannot price. The count also has to be
    // the model's real layer count, and `require_expert_offload_engaged`
    // asserts that it resolved to exactly `n_layer`: with every layer REQUESTED
    // resident, the expert override is the only thing left that can put trunk
    // weights on the host, which is what makes the VRAM reading mean something.
    //
    // context_length is the shared kContextLength (8192), NOT the 4096 of the
    // prototype's illustrative snippet. This case is a controlled comparison
    // against [mtp-a4b] on the same card, and a controlled comparison changes
    // ONE variable; max_tokens 256 matches it for the same reason.
    //
    // NO expectation is stated here, deliberately. Whether an experts-only
    // split moves MTP's -0.92 % is the open question #42(iii) names, and a
    // case that printed the answer it wanted before measuring would be the
    // fourth way this issue has gone wrong. The residency rule that judges it
    // is chosen by the engine (decision #74) and, because the experts are
    // host-side, it is the PARTIAL rule — 31 of 31 layers notwithstanding.
    // 30, not 31: gemma4 26B-A4B has THIRTY layers. The first run of this case
    // asked for 31 and the engine refused it — "cpu_moe_layers=31 exceeds the
    // model's 30 layers … it is not clamped, because a count that cannot be
    // honoured is a configuration error, not a preference" — which is the
    // refusal working, and the count being wrong. #42(b)'s "18 of 31" prose is
    // the same slip; the JSON it was taken from recorded 18/30.
    //
    // MEASURED 2026-09-22 on the 1080 Ti, sweeping cpu_moe_layers by hand
    // (1080 Ti, gpu_layers=30, ctx 8192, max_tokens 256, 4 trials):
    //   N=30  2767 MiB VRAM   plain 17.68  mtp 18.18   effect -0.29 % (floor 3.11 %)
    //   N=20  6775 MiB VRAM   plain 22.62  mtp 23.76   effect +4.16 % (floor 0.83 %)
    //   N=12  9446 MiB VRAM   — engagement assertion refused the run, see below
    // Two things fall out. Throughput rises as experts come BACK onto the card
    // (18.2 -> 22.6 plain), and MTP's benefit rises with it (-0.29 % -> +4.16 %),
    // which is decision #74's resident-fraction rule showing up inside a single
    // model. Whole-layer offload at 18/30 measured 18.7 tok/s (#42(b)), so
    // experts-on-host at N=30 buys nothing by itself — its value is the 5 GB of
    // VRAM it frees, which only pays once that VRAM holds experts again.
    //
    // N is 30 here, not the faster 20, deliberately: this case is a STABLE
    // comparison against #42(b), and `require_expert_offload_engaged`'s bound
    // (free - overflow) assumes most weights are host-side. At N=20 the load
    // lands 300 MiB under that bound and at N=12 it exceeds it — not because
    // the knob failed, but because 18 layers of experts ARE resident and the
    // bound cannot tell "engaged and partly resident" from "never engaged".
    // Sweeping N is gh#167/gh#180's job and needs a residency-aware bound.
    gh153::run_four_arm_bench(
        {"a4b_experts", "gemma4_a4b_qat", "mtp_a4b", "30", 256, 4, 30});
}

TEST_CASE("gh#153 MTP vs plain decode throughput — four arms, Gemma 4 26B-A4B "
          "at IQ2_M, the quant that fits an 11 GiB card whole",
          "[.][model][gh153][benchmark][mtp-a4b-iq2]") {
    // The "make it fit" lever against [mtp-a4b]'s "place it better" one, on
    // the same card, same prompt, same arms, same floor.
    //
    // Every A4B figure before this one was taken PARTIALLY resident, because
    // the QAT Q4 trunk is 14.25 GB against 11 GiB: 18.7 tok/s whole-layer
    // (#42(b)), 18.2 with every expert on the host, 22.6 once VRAM bought
    // experts back (#75). MTP was worth ~nothing in all of them, while it is
    // worth +42.8 % on a fully resident E4B (#42(a)). #74's rule says the
    // resident FRACTION is what governs that — so the test of the rule is a
    // model of this class that fits ENTIRELY.
    //
    // UD-IQ2_M is 9.33 GiB, so `gpu_layers: -1` prices as `Offload::full`
    // against a ~10.8 GiB budget and the gh#148 admission gate ADMITS it —
    // the opposite of the -1/99 trap the experts case documents, and the
    // reason this case can say -1 where that one must not.
    //
    // Because it is fully resident the engine selects the STRICT rule: MTP
    // must CLEAR the same-config floor, exactly as the E4B case is judged.
    // That is the point. If MTP pays here, the resident-fraction rule is
    // vindicated on a MoE and not only across two different models; if it
    // does not, #42's hypothesis needs the MoE-routing half after all, and
    // this case is the evidence for that.
    //
    // NOT QAT: no QAT IQ2 exists upstream (the QAT repo ships Q4_K_XL only),
    // so this trades QAT fidelity for residency. ~2.5 bits/weight on a model
    // that is already sparse degrades STRUCTURED OUTPUT first, which a tok/s
    // figure cannot see — a fast number here is a reason to check tool-call
    // formatting, not a reason to ship the quant.
    //
    // WHY n_ubatch: 128. The first attempt at this case (e851a7f) put the
    // weights on the card and died allocating the compute buffer:
    //
    //   free      10465 MiB   the card minus a ~700 MiB desktop session
    //   weights    9552 MiB   loaded fine
    //   KV           84 MiB   q4_0, iSWA
    //   compute     527 MiB   ubatch 512   <- cudaMalloc failed here
    //
    // On paper that is 10163 of 10465 and it STILL failed, so at least 302 MiB
    // of real demand is invisible to those four numbers — CUDA context,
    // allocator granularity, a desktop that grows while 9.3 GiB uploads. That
    // 302 is a measured LOWER bound; nothing establishes its ceiling.
    //
    // The compute buffer is dominated by the ubatch-sized logits tensor
    // (n_ubatch x 262144 vocab x 4 B is 512 MiB at ubatch 512, essentially the
    // whole 527), so it scales very nearly linearly:
    //   512 -> ~527 MiB     256 -> ~264 MiB     128 -> ~132 MiB
    //
    // 256 is not enough, and the term the first attempt's arithmetic left out
    // is why: the MTP head. build_mtp_head loads it at the TIER's gpu_layers
    // (-1, so onto the card: ~229 MiB of weights) and builds its context from
    // the TIER's cparams, so it pays a vocab-sized buffer of its own. Against
    // the same 10465, with the head included:
    //   ubatch 256:  9552 + 84 + 264 + 229 + ~250 = ~10379  -> ~86 MiB spare
    //   ubatch 128:  9552 + 84 + 132 + 229 + ~125 = ~10122  -> ~343 MiB spare
    // 86 MiB is well under the >=302 MiB the failure already proved goes
    // unaccounted for, so at 256 this would most likely OOM again — at the
    // head rather than at the trunk, which is a worse failure to read. 128 is
    // the smallest value config.h calls productive and is the first one with
    // margin of the same order as the unexplained term.
    //
    // It costs the figures nothing. The prompt is ~300 tokens, so prefill is a
    // couple more micro-batches inside a call dominated by 256 decoded tokens,
    // and all four arms carry the same value — the floor and the effect are
    // intra-case, so neither can move because of it.
    gh153::run_four_arm_bench(
        // label, trunk, head, gpu_layers, max_tokens, measured_rounds,
        // cpu_moe_layers (0 — every layer whole and on the card), n_ubatch.
        {"a4b_iq2", "gemma4_a4b_iq2", "mtp_a4b", "-1", 256, 4, 0, 128});
}

// gh#193: the figure the warm-decode arms above cannot see.
//
// A consumer showed from log timestamps that entropic's reported ms spans
// prefill AND decode, and that in a real agentic turn prefill dominates —
// ~24k prefill tokens against ~4.7k generated, with 516s of a 633s turn in
// three cold generations. Our bench prompts are short and its trials repeat,
// so every figure it produces is warm decode.
//
// It is also the term v2.13.1's MoE ordering rests on. `gpu_layers: auto`
// moves experts host-side before it drops a layer because attention is what
// prefill leans on — an argument this repository had never measured.
//
// The A4B at IQ2 is the right subject: it is the one model here that can be
// fully resident AND partially offloaded on this card, so residency is the
// only variable that moves between levels.
TEST_CASE("gh#193: what cold prefill costs at each residency level",
          "[benchmark][.][prefill-residency]") {
    gh153::run_prefill_residency_bench(
        // label, trunk, head, gpu_layers/n_ubatch replaced per point,
        // max_tokens, measured_rounds, cpu_moe_layers, n_ubatch.
        {"a4b_iq2_prefill", "gemma4_a4b_iq2", "mtp_a4b", "-1", 1, 1, 0, 128},
        // {gpu_layers, n_ubatch}. Compute buffers scale with n_ubatch and
        // compete with weights for the same VRAM: ~1222 MiB per context at
        // 512 against ~305 at 128, doubled by the MTP head's second context.
        // At ~303 MiB per A4B-IQ2 layer that is about six layers.
        //
        // The first three points are the ISO-VRAM FRONTIER — each buys its
        // ubatch by giving up layers, so they are the choice `auto` would
        // actually be making if it chose ubatch at all. The last two hold
        // layers FIXED at 24 to isolate ubatch's own effect from the
        // residency it costs.
        {{"-1", 128}, {"28", 256}, {"24", 512},
         {"24", 128}, {"24", 256}});
}
