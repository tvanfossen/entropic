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
#include <cstdio>

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

    // temperature > 0 → loud (lossless only at greedy). Errors before any head
    // load, so the message must be present and the code exact.
    {
        entropic::GenerationParams p;
        p.max_tokens = 16;
        p.temperature = 0.7f;
        auto r = backend.generate_mtp(msgs, p, no_stream, cancel, head.string(), 16);
        std::printf("\n[gh108] temp>0 → code=%d msg=%s\n", r.error_code,
                    r.error_message.c_str());
        REQUIRE(r.error_code == INCOMPAT);
        REQUIRE_FALSE(r.error_message.empty());
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
    // tools staged → loud (sequential-tool stop + parse snapshot bypassed).
    {
        backend.set_active_tools(gh87verify::read_file_tool_json());
        entropic::GenerationParams p;
        p.max_tokens = 16;
        p.temperature = 0.0f;
        auto r = backend.generate_mtp(msgs, p, no_stream, cancel, head.string(), 16);
        REQUIRE(r.error_code == INCOMPAT);
        backend.set_active_tools("");  // clear for the greedy case below
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
