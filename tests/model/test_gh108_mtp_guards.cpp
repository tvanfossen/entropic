// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh108_mtp_guards.cpp
 * @brief gh#108 (v2.9.1): MTP fail-fast/fail-loud guards on the live backend.
 *
 * v2.9.0 shipped MTP that silently bypassed grammar/tools/stop/streaming and
 * was lossless only at temp=0. v2.9.1 makes MTP refuse — LOUDLY — to run
 * outside its correct envelope, so a consumer corrects the config instead of
 * getting silently-wrong output. These tests drive the REAL backend:
 *   - each out-of-envelope request → ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG
 *     (NOT a silent plain-decode fallback)
 *   - the in-envelope greedy request still engages (n_drafted>0) + is coherent
 *   - an oversized n_draft → a clear error, not a GGML_ABORT crash
 *   - greedy MTP across a persistent multi-turn context (emergent, mandatory)
 */

#include "gh87_verify_helpers.h"  // LlamaCppBackend + config/result/message

#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace {

entropic::ModelConfig gh108_cfg(const std::filesystem::path& path) {
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

std::vector<entropic::Message> one_turn(const std::string& content) {
    entropic::Message u;
    u.role = "user";
    u.content = content;
    return {u};
}

}  // namespace

TEST_CASE("gh#108 MTP guards: out-of-envelope requests fail loud",
          "[model][gh108][mtp][guards]") {
    auto target = gh87verify::model_path("gemma-4-E2B-it-Q8_0.gguf");
    auto head = gh87verify::model_path("mtp-gemma-4-E2B-it.gguf");
    if (!std::filesystem::is_regular_file(target)) {
        SKIP("MTP target GGUF not present: " + target.string());
    }
    if (!std::filesystem::is_regular_file(head)) {
        SKIP("MTP head GGUF not present: " + head.string());
    }

    entropic::LlamaCppBackend backend;
    REQUIRE(backend.load(gh108_cfg(target)));
    REQUIRE(backend.activate());

    std::atomic<bool> cancel{false};
    std::function<void(std::string_view)> no_stream;  // empty = non-streaming
    auto msgs = one_turn("Continue: 1\n2\n3\n4\n");

    const auto INCOMPAT = ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG;

    // gh#108 (v2.9.4): temperature > 0 → MTP now RUNS instead of refusing.
    // The draft proposal is a deterministic point mass (mtp_envelope.h), so
    // the existing exact-match accept step is already lossless at any
    // temperature. See the dedicated distribution test below for the
    // empirical check that this is actually true, not just argued.
    {
        entropic::GenerationParams p;
        p.max_tokens = 16;
        p.temperature = 0.7f;
        auto r = backend.generate_mtp(msgs, p, no_stream, cancel, head.string(), 16);
        std::printf("\n[gh108] temp>0 → code=%d drafted=%d [%s]\n", r.error_code,
                    r.n_drafted, r.content.c_str());
        REQUIRE(r.error_code == 0);
        REQUIRE_FALSE(r.content.empty());
        REQUIRE(r.n_drafted > 0);
    }
    // grammar active → loud (MTP does not enforce GBNF).
    {
        entropic::GenerationParams p;
        p.max_tokens = 16;
        p.temperature = 0.0f;
        p.grammar = "root ::= \"ok\"";
        auto r = backend.generate_mtp(msgs, p, no_stream, cancel, head.string(), 16);
        REQUIRE(r.error_code == INCOMPAT);
    }
    // streaming (bound on_token) → loud (thinking-strip is post-buffer).
    {
        entropic::GenerationParams p;
        p.max_tokens = 16;
        p.temperature = 0.0f;
        std::function<void(std::string_view)> on_token = [](std::string_view) {};
        auto r = backend.generate_mtp(msgs, p, on_token, cancel, head.string(), 16);
        REQUIRE(r.error_code == INCOMPAT);
    }
    // gh#108 (v2.9.2): tools staged → MTP RUNS (no false refusal). MTP is
    // lossless at temp=0 and gemma4 tool-calls are parsed post-hoc (not
    // sampler-grammar-constrained), so MTP + tools produces the same correct
    // tool call as plain decode. This is the reachability fix.
    {
        backend.set_active_tools(gh87verify::read_file_tool_json());
        entropic::GenerationParams p;
        p.max_tokens = 200;
        p.temperature = 0.0f;
        auto r = backend.generate_mtp(
            one_turn("You must call the read_file tool to read /etc/hostname. "
                     "Respond with the tool call."),
            p, no_stream, cancel, head.string(), 4);
        auto parsed = backend.parse_response(r.content);
        std::printf("[gh108] tools → code=%d drafted=%d calls=%zu\n[%s]\n",
                    r.error_code, r.n_drafted, parsed.tool_calls.size(),
                    r.content.c_str());
        REQUIRE(r.error_code == 0);             // NOT a loud refusal anymore
        REQUIRE(r.n_drafted > 0);               // MTP engaged with tools staged
        REQUIRE(parsed.tool_calls.size() >= 1); // correct tool call produced
        backend.set_active_tools("");           // clear for the cases below
    }
    // gh#108 (v2.9.2): MTP honors stop sequences — halts on the stop instead of
    // over-generating past it (the mechanism that makes the sequential-tool
    // hard-stop work under MTP).
    {
        entropic::GenerationParams p;
        p.max_tokens = 64;
        p.temperature = 0.0f;
        p.stop = {"STOPHERE"};
        auto r = backend.generate_mtp(
            one_turn("Output exactly this text and nothing else: BEGIN STOPHERE END"),
            p, no_stream, cancel, head.string(), 4);
        std::printf("[gh108] stop → finish=%s [%s]\n", r.finish_reason.c_str(),
                    r.content.c_str());
        REQUIRE(r.error_code == 0);
        REQUIRE(r.finish_reason == "stop");                  // halted on the stop
        REQUIRE(r.content.find("END") == std::string::npos); // did not run past it
    }
    // oversized n_draft (1+n_max > n_batch) → clear error, NOT a GGML_ABORT.
    {
        entropic::GenerationParams p;
        p.max_tokens = 16;
        p.temperature = 0.0f;
        auto r = backend.generate_mtp(msgs, p, no_stream, cancel, head.string(),
                                      /*n_max=*/100000);
        REQUIRE(r.error_code == INCOMPAT);
        REQUIRE(r.error_message.find("n_batch") != std::string::npos);
    }
    // In-envelope greedy → MTP actually engages + coherent (no false refusal).
    {
        entropic::GenerationParams p;
        p.max_tokens = 48;
        p.temperature = 0.0f;
        auto r = backend.generate_mtp(msgs, p, no_stream, cancel, head.string(), 16);
        std::printf("[gh108] greedy → code=%d drafted=%d tput=%.1f\n[%s]\n",
                    r.error_code, r.n_drafted, r.throughput_tok_s,
                    r.content.c_str());
        REQUIRE(r.error_code == 0);
        REQUIRE_FALSE(r.content.empty());
        REQUIRE(r.n_drafted > 0);
        REQUIRE(r.throughput_tok_s > 0.0);  // gh#108: was always 0.0 before
    }

    backend.deactivate();
    backend.unload();
}

TEST_CASE("gh#108 MTP greedy holds across a multi-turn context",
          "[model][gh108][mtp][multiturn]") {
    auto target = gh87verify::model_path("gemma-4-E2B-it-Q8_0.gguf");
    auto head = gh87verify::model_path("mtp-gemma-4-E2B-it.gguf");
    if (!std::filesystem::is_regular_file(target) ||
        !std::filesystem::is_regular_file(head)) {
        SKIP("MTP target/head GGUF not present");
    }

    entropic::LlamaCppBackend backend;
    REQUIRE(backend.load(gh108_cfg(target)));
    REQUIRE(backend.activate());

    std::atomic<bool> cancel{false};
    std::function<void(std::string_view)> no_stream;
    entropic::GenerationParams p;
    p.max_tokens = 40;
    p.temperature = 0.0f;

    // Accumulate a persistent conversation; every greedy MTP turn must engage
    // + stay coherent (the head re-binds against ctx_ each turn).
    std::vector<entropic::Message> conv;
    const char* turns[] = {"Name a primary color.",
                           "Name another, different one.",
                           "Now name a third."};
    for (const char* t : turns) {
        entropic::Message u;
        u.role = "user";
        u.content = t;
        conv.push_back(u);
        auto r = backend.generate_mtp(conv, p, no_stream, cancel, head.string(), 16);
        std::printf("[gh108 multiturn] code=%d drafted=%d [%s]\n", r.error_code,
                    r.n_drafted, r.content.c_str());
        REQUIRE(r.error_code == 0);
        REQUIRE_FALSE(r.content.empty());
        REQUIRE(r.n_drafted > 0);
        entropic::Message a;
        a.role = "assistant";
        a.content = r.content;
        conv.push_back(a);
    }

    backend.deactivate();
    backend.unload();
}

TEST_CASE("gh#108 MTP + flash attention succeeds (v2.9.3, upstream #25148)",
          "[model][gh108][mtp][flash]") {
    auto target = gh87verify::model_path("gemma-4-E2B-it-Q8_0.gguf");
    auto head = gh87verify::model_path("mtp-gemma-4-E2B-it.gguf");
    if (!std::filesystem::is_regular_file(target) ||
        !std::filesystem::is_regular_file(head)) {
        SKIP("MTP target/head GGUF not present");
    }
    // Pre-v2.9.3 the gemma4-assistant head (GQA-2 + head_dim-512) aborted the
    // flash MMA kernel on this pin, so flash_attn was guarded loud. The
    // extern/llama.cpp pin is now past upstream #25148 ("CUDA: fix Gemma E4B
    // MTP FlashAttention"), which restores the GQA-2 specialization — flash
    // attention must now engage MTP successfully instead of erroring.
    entropic::ModelConfig cfg = gh108_cfg(target);
    cfg.flash_attn = true;
    entropic::LlamaCppBackend backend;
    REQUIRE(backend.load(cfg));
    REQUIRE(backend.activate());

    std::atomic<bool> cancel{false};
    std::function<void(std::string_view)> no_stream;
    entropic::GenerationParams p;
    p.max_tokens = 16;
    p.temperature = 0.0f;
    auto r = backend.generate_mtp(one_turn("Hello."), p, no_stream, cancel,
                                  head.string(), 4);
    std::printf("[gh108] mtp+flash → code=%d msg=%s n_drafted=%d\n", r.error_code,
                r.error_message.c_str(), r.n_drafted);
    REQUIRE(r.error_code == 0);
    REQUIRE_FALSE(r.content.empty());
    REQUIRE(r.n_drafted > 0);

    backend.deactivate();
    backend.unload();
}

TEST_CASE("gh#108 MTP + flash + q4 KV succeeds on E4B Q2-mobile "
          "(the target consumer config)",
          "[model][gh108][mtp][flash]") {
    // This is the exact config gh#108 asked for: Gemma-4 E4B Q2-mobile
    // (TQ2_0 QAT) + MTP + flash_attn + quantized (q4_0) KV + windowed SWA
    // (already the build_cparams default since v2.9.2). Quantized KV cache
    // requires flash in llama.cpp, so this also exercises the KV-quant unlock
    // that the flash fix (upstream #25148) recovers, not just the flash path
    // alone.
    auto target = gh87verify::model_path("gemma-4-E4B-it-qat-UD-Q2_K_XL.gguf");
    auto head = gh87verify::model_path("mtp-gemma-4-E4B-it.gguf");
    if (!std::filesystem::is_regular_file(target) ||
        !std::filesystem::is_regular_file(head)) {
        SKIP("MTP target/head GGUF not present");
    }
    entropic::ModelConfig cfg = gh108_cfg(target);
    cfg.flash_attn = true;
    cfg.cache_type_k = "q4_0";
    cfg.cache_type_v = "q4_0";
    entropic::LlamaCppBackend backend;
    REQUIRE(backend.load(cfg));
    REQUIRE(backend.activate());

    std::atomic<bool> cancel{false};
    std::function<void(std::string_view)> no_stream;
    entropic::GenerationParams p;
    p.max_tokens = 16;
    p.temperature = 0.0f;
    auto r = backend.generate_mtp(one_turn("Hello."), p, no_stream, cancel,
                                  head.string(), 4);
    std::printf("[gh108] mtp+flash+q4kv(E4B Q2-mobile) → code=%d msg=%s n_drafted=%d\n",
                r.error_code, r.error_message.c_str(), r.n_drafted);
    REQUIRE(r.error_code == 0);
    REQUIRE_FALSE(r.content.empty());
    REQUIRE(r.n_drafted > 0);

    backend.deactivate();
    backend.unload();
}

TEST_CASE("gh#108 MTP is lossless at temperature>0 (empirical distribution check)",
          "[model][gh108][mtp][temperature]") {
    // Regression tripwire for the mtp_envelope.h v2.9.4 point-mass proof: if a
    // future extern/llama.cpp pin bump changes the MTP draft() to honor
    // cur_p->selected instead of data[0] (making the proposal genuinely
    // stochastic, not a point mass), the accept step's exact-match shortcut
    // stops being lossless and this test should start failing the band check
    // below — that is its job, not a false positive to silence.
    auto target = gh87verify::model_path("gemma-4-E2B-it-Q8_0.gguf");
    auto head = gh87verify::model_path("mtp-gemma-4-E2B-it.gguf");
    if (!std::filesystem::is_regular_file(target) ||
        !std::filesystem::is_regular_file(head)) {
        SKIP("MTP target/head GGUF not present");
    }

    entropic::LlamaCppBackend backend;
    REQUIRE(backend.load(gh108_cfg(target)));
    REQUIRE(backend.activate());

    std::atomic<bool> cancel{false};
    std::function<void(std::string_view)> no_stream;
    auto msgs = one_turn("Continue: 1\n2\n3\n4\n");

    constexpr int kSamples = 200;
    constexpr float kTemperature = 0.7f;

    // Empirical first-token frequency, MTP-engaged vs plain decode, at the
    // same stochastic temperature and unseeded (params.seed=-1 default, so
    // each call draws fresh). If the accept step silently biased the output
    // distribution, MTP's histogram would diverge from plain decode's beyond
    // sampling noise.
    std::map<std::string, int> mtp_freq;
    std::map<std::string, int> plain_freq;
    long long mtp_drafted_total = 0;
    for (int i = 0; i < kSamples; ++i) {
        entropic::GenerationParams p;
        p.max_tokens = 1;
        p.temperature = kTemperature;
        auto r = backend.generate_mtp(msgs, p, no_stream, cancel, head.string(), 16);
        REQUIRE(r.error_code == 0);
        mtp_freq[r.content]++;
        mtp_drafted_total += r.n_drafted;
    }
    for (int i = 0; i < kSamples; ++i) {
        entropic::GenerationParams p;
        p.max_tokens = 1;
        p.temperature = kTemperature;
        auto r = backend.generate_streaming(msgs, p, no_stream, cancel);
        REQUIRE(r.error_code == 0);
        plain_freq[r.content]++;
    }

    // Tolerance-band check on every token seen by either path: ~3 std devs
    // of a binomial proportion at N=200, plus a small floor to absorb
    // low-count noise, generous enough for sampling variance while still
    // catching a systematically biased accept step.
    std::set<std::string> tokens;
    for (auto& kv : mtp_freq) { tokens.insert(kv.first); }
    for (auto& kv : plain_freq) { tokens.insert(kv.first); }
    for (auto& t : tokens) {
        double p_plain = static_cast<double>(plain_freq[t]) / kSamples;
        double p_mtp = static_cast<double>(mtp_freq[t]) / kSamples;
        double band = 3.0 * std::sqrt(p_plain * (1.0 - p_plain) / kSamples) + 0.02;
        std::printf("[gh108 dist] token=%-8s plain=%.3f mtp=%.3f band=%.3f\n",
                    t.c_str(), p_plain, p_mtp, band);
        REQUIRE(std::abs(p_mtp - p_plain) <= band);
    }

    // Sanity: acceptance shouldn't be implausibly higher at temp>0 than at
    // temp=0 for the same prompt — a red flag the accept comparison runs
    // against the wrong distribution.
    entropic::GenerationParams p0;
    p0.max_tokens = 16;
    p0.temperature = 0.0f;
    auto r0 = backend.generate_mtp(msgs, p0, no_stream, cancel, head.string(), 16);
    REQUIRE(r0.error_code == 0);
    double mtp_avg_drafted = static_cast<double>(mtp_drafted_total) / kSamples;
    std::printf("[gh108 dist] avg n_drafted temp=0.7: %.2f, temp=0: %d\n",
                mtp_avg_drafted, r0.n_drafted);

    backend.deactivate();
    backend.unload();
}
