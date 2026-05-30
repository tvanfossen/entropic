// SPDX-License-Identifier: Apache-2.0
/**
 * @file backend_real_model_smoke_test.cpp
 * @brief v2.3.10 unit-scope smoke against LlamaCppBackend with a tiny
 *        real model (CPU). Drives load → activate → generate →
 *        deactivate → unload paths that vocab-only smoke cannot reach.
 *
 * Skips gracefully (WARN) if the small model is not present at
 * `$HOME/.entropic/models/Qwen3.5-0.8B-Q8_0.gguf`. Required for the
 * v2.3.10 inference coverage gate to pass in local dev — CI may need
 * the model cached or the gate adjusted, but that's a separate
 * concern.
 *
 * Design note: each Catch2 BDD AND_WHEN re-runs its parent WHEN/GIVEN
 * tree, which means a nested test triggers multiple model loads. The
 * 0.8B model loads cleanly but repeated load/unload cycles under CUDA
 * destructors hit a SEGV in llama.cpp's shutdown path. Tests here use
 * flat TEST_CASE blocks (one backend lifecycle per case) instead.
 *
 * @version 2.3.10
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/inference/backend.h>
#include <entropic/types/config.h>
#include <entropic/types/generation_result.h>
#include <entropic/types/message.h>

#include "../../../src/inference/llama_cpp_backend.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path home_model(const std::string& name) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return {};
    }
    return std::filesystem::path(home) / ".entropic" / "models" / name;
}

entropic::ModelConfig make_cpu_config(const std::filesystem::path& gguf) {
    entropic::ModelConfig cfg;
    cfg.path = gguf;
    cfg.adapter = "qwen35";
    cfg.context_length = 512;
    cfg.gpu_layers = 0;       // CPU only
    cfg.keep_warm = false;
    cfg.use_mlock = false;    // avoid CAP_IPC_LOCK in CI
    cfg.flash_attn = false;   // CPU path
    cfg.n_batch = 64;
    cfg.n_threads = 2;
    cfg.cache_type_k = "f16";
    cfg.cache_type_v = "f16";
    return cfg;
}

bool small_model_path(std::filesystem::path& out) {
    out = home_model("Qwen3.5-0.8B-Q8_0.gguf");
    return !out.empty() && std::filesystem::exists(out);
}

}  // namespace

// ── Single-shot lifecycle: load → activate → many ops → unload ────

TEST_CASE("LlamaCppBackend lifecycle on a small real model (one load)",
          "[v2.3.10][inference][real_model_smoke]") {
    std::filesystem::path path;
    if (!small_model_path(path)) {
        WARN("small model missing — skipping real-model smoke");
        return;
    }

    entropic::LlamaCppBackend backend;
    REQUIRE(backend.state() == entropic::ModelState::COLD);

    // Load
    auto cfg = make_cpu_config(path);
    REQUIRE(backend.load(cfg));
    REQUIRE(backend.state() == entropic::ModelState::WARM);
    REQUIRE(backend.is_loaded());
    REQUIRE_FALSE(backend.is_active());

    // Activate
    REQUIRE(backend.activate());
    REQUIRE(backend.state() == entropic::ModelState::ACTIVE);
    REQUIRE(backend.is_active());
    REQUIRE(backend.context_length() > 0);

    // Info / capabilities
    auto info = backend.info();
    REQUIRE_FALSE(info.name.empty());
    auto caps = backend.capabilities();
    REQUIRE_FALSE(caps.empty());
    using C = entropic::BackendCapability;
    for (auto cap : {C::KV_CACHE, C::STREAMING, C::TOKENIZER,
                     C::GRAMMAR, C::PROMPT_CACHING, C::LOG_PROBS}) {
        (void)backend.supports(cap);
    }

    // Tokenize / count
    auto tokens = backend.tokenize_text("Hello there");
    REQUIRE_FALSE(tokens.empty());
    REQUIRE(backend.count_tokens("Hello there") > 0);

    // complete() — short prompt, tiny max_tokens. Coverage, not quality.
    {
        entropic::GenerationParams params;
        params.max_tokens = 4;
        params.temperature = 0.0f;
        params.enable_thinking = false;
        auto result = backend.complete("Hi", params);
        (void)result;
    }

    // generate_streaming() — drives per-token callback path.
    {
        int call_count = 0;
        auto on_tok = [&](std::string_view t) {
            ++call_count;
            (void)t;
        };
        std::vector<entropic::Message> msgs;
        entropic::Message u;
        u.role = "user";
        u.content = "hi";
        msgs.push_back(u);

        entropic::GenerationParams params;
        params.max_tokens = 3;
        params.temperature = 0.0f;
        params.enable_thinking = false;
        std::atomic<bool> cancel{false};
        auto result = backend.generate_streaming(msgs, params, on_tok, cancel);
        (void)result;
        (void)call_count;
    }

    // min_p sampler branch (v2.3.10 gh#23) — exercises the new sampler chain entry.
    {
        entropic::GenerationParams params;
        params.max_tokens = 2;
        params.temperature = 0.7f;
        params.min_p = 0.1f;
        params.enable_thinking = false;
        auto result = backend.complete("hi", params);
        (void)result;
    }

    // evaluate_logprobs — drives eval_mutex_ + do_evaluate_logprobs decode/logit path.
    {
        auto eval_tokens = backend.tokenize_text("abc");
        if (eval_tokens.size() >= 2) {
            auto lp = backend.evaluate_logprobs(
                eval_tokens.data(), static_cast<int>(eval_tokens.size()));
            (void)lp;
        }
    }

    // compute_perplexity — wrapper around evaluate_logprobs, drives the perplexity reducer.
    {
        auto pp_tokens = backend.tokenize_text("hello world");
        if (pp_tokens.size() >= 2) {
            float pp = backend.compute_perplexity(
                pp_tokens.data(), static_cast<int>(pp_tokens.size()));
            (void)pp;
        }
    }

    // save_state / restore_state / clear_state round-trip on seq 0 — drives KV save+restore paths.
    {
        std::vector<uint8_t> buf;
        bool saved = backend.save_state(0, buf);
        if (saved && !buf.empty()) {
            (void)backend.clear_state(0);
            bool restored = backend.restore_state(0, buf);
            (void)restored;
        }
    }

    // complete() with grammar set — drives the grammar branch in create_sampler / sampler chain.
    {
        entropic::GenerationParams params;
        params.max_tokens = 2;
        params.temperature = 0.0f;
        params.enable_thinking = false;
        // Minimal GBNF: any single character — exercises grammar branch without constraining.
        params.grammar = "root ::= [a-zA-Z0-9 \\n\\t.,!?]+";
        auto result = backend.complete("hi", params);
        (void)result;
    }

    // gh#86 (v2.6.1): generate() (NOT complete()) drives apply_chat_template,
    // which renders the GGUF jinja template with enable_thinking threaded.
    // complete() is raw text completion and never touches the template, so
    // the pre-v2.6.1 version of this block (complete() + a false
    // "drives the thinking branch" comment + a discarded result) exercised
    // nothing. Assert a real outcome on both thinking settings.
    {
        std::vector<entropic::Message> msgs;
        entropic::Message u;
        u.role = "user";
        u.content = "hi";
        msgs.push_back(u);

        entropic::GenerationParams params;
        params.max_tokens = 2;
        params.temperature = 0.0f;

        params.enable_thinking = true;
        auto think_on = backend.generate(msgs, params);
        CHECK(think_on.error_message.empty());

        params.enable_thinking = false;
        auto think_off = backend.generate(msgs, params);
        CHECK(think_off.error_message.empty());
    }

    // gh#87 (v2.7.0): common_chat tool-call render + parse paths.
    // set_active_tools → render_with_tools (captures parse params) →
    // has_common_chat_params / common_chat_parse_reliable (the routing
    // getters) → generate() through the tools render seam →
    // parse_response (reconstructs the captured PEG parser). Qwen3.5-0.8B
    // routes parsing to the adapter (common_chat_parse_reliable is false —
    // not gemma4), but every render/capture/getter/parse body runs here;
    // this is the only in-gate exercise of the Phase D backend surface.
    {
        const std::string tools_json =
            R"([{"name":"read_file","description":"Read a file",)"
            R"("inputSchema":{"type":"object","properties":)"
            R"({"path":{"type":"string"}},"required":["path"]}}])";
        backend.set_active_tools(tools_json);

        std::vector<entropic::Message> tmsgs;
        entropic::Message tu;
        tu.role = "user";
        tu.content = "Read config.yaml";
        tmsgs.push_back(tu);

        entropic::GenerationParams tparams;
        tparams.max_tokens = 4;
        tparams.temperature = 0.0f;
        tparams.enable_thinking = false;

        // render_with_tools — routes the staged defs into common_chat and
        // captures (or low-level-template falls back).
        auto rendered = backend.render_with_tools(tmsgs, tparams);
        CHECK_FALSE(rendered.empty());

        // Routing getters the orchestrator/interface query post-generation.
        (void)backend.has_common_chat_params();
        (void)backend.common_chat_parse_reliable();

        // generate() with tools staged — render_prompt routes through the
        // tools render seam rather than apply_chat_template.
        auto tool_gen = backend.generate(tmsgs, tparams);
        CHECK(tool_gen.error_message.empty());

        // parse_response — reconstruct captured parser params + common_chat
        // parse on a synthetic native emission (content-only if no capture).
        auto parsed = backend.parse_response(
            "<function=read_file>\n<parameter=path>config.yaml"
            "</parameter>\n</function>");
        (void)parsed.tool_calls;
        (void)parsed.content;
        (void)parsed.reasoning_content;

        // Clear staged tools (the empty / clear path).
        backend.set_active_tools("");
    }

    // count_tokens on a longer string — drives the non-trivial tokenizer length path.
    {
        std::string long_input;
        for (int i = 0; i < 32; ++i) {
            long_input += "The quick brown fox jumps over the lazy dog. ";
        }
        int n = backend.count_tokens(long_input);
        (void)n;
    }

    // Full capability enum sweep — exercises every static + dynamic branch in do_supports.
    {
        using C = entropic::BackendCapability;
        for (auto cap : {C::KV_CACHE, C::HIDDEN_STATE, C::STREAMING,
                         C::RAW_COMPLETION, C::GRAMMAR, C::LORA_ADAPTERS,
                         C::MULTI_SEQUENCE, C::TOKENIZER, C::LOG_PROBS,
                         C::VISION, C::SPECULATIVE_DECODING,
                         C::PROMPT_CACHING, C::AUDIO}) {
            (void)backend.supports(cap);
        }
    }

    // generate_seq / generate_streaming_seq — drive the seq_id overloads (default seq 0).
    {
        std::vector<entropic::Message> msgs;
        entropic::Message u;
        u.role = "user";
        u.content = "hi";
        msgs.push_back(u);

        entropic::GenerationParams params;
        params.max_tokens = 2;
        params.temperature = 0.0f;
        params.enable_thinking = false;

        auto r1 = backend.generate_seq(0, msgs, params);
        (void)r1;

        std::atomic<bool> cancel{false};
        auto on_tok = [](std::string_view) {};
        auto r2 = backend.generate_streaming_seq(0, msgs, params, on_tok, cancel);
        (void)r2;
    }

    // generate() (non-streaming, message-shaped) — drives do_generate text-only path.
    {
        std::vector<entropic::Message> msgs;
        entropic::Message u;
        u.role = "user";
        u.content = "hi";
        msgs.push_back(u);

        entropic::GenerationParams params;
        params.max_tokens = 2;
        params.temperature = 0.0f;
        params.enable_thinking = false;
        auto result = backend.generate(msgs, params);
        (void)result;
    }

    // generate_speculative without a draft pair — drives the abstract NOT_SUPPORTED path.
    {
        std::vector<entropic::Message> msgs;
        entropic::Message u;
        u.role = "user";
        u.content = "hi";
        msgs.push_back(u);

        entropic::GenerationParams params;
        params.max_tokens = 2;
        params.temperature = 0.0f;
        params.enable_thinking = false;
        std::atomic<bool> cancel{false};
        auto on_tok = [](std::string_view) {};
        auto result = backend.generate_speculative(msgs, params, on_tok, cancel);
        (void)result;
    }

    // Multi-message chat history — drives apply_chat_template multi-turn path.
    {
        std::vector<entropic::Message> msgs;
        entropic::Message m;
        m.role = "user"; m.content = "Hi"; msgs.push_back(m);
        m.role = "assistant"; m.content = "Hello!"; msgs.push_back(m);
        m.role = "user"; m.content = "Bye"; msgs.push_back(m);
        entropic::GenerationParams params;
        params.max_tokens = 2;
        params.temperature = 0.0f;
        auto r = backend.complete("ignored", params);
        (void)r;
        auto r2 = backend.generate(msgs, params);
        (void)r2;
    }

    // top_k=1 (greedy-via-top-k) + repeat_penalty>1 — drives extra sampler stages.
    {
        entropic::GenerationParams params;
        params.max_tokens = 2;
        params.temperature = 0.7f;
        params.top_k = 1;
        params.repeat_penalty = 1.1f;
        params.enable_thinking = false;
        auto r = backend.complete("hi", params);
        (void)r;
    }

    // Non-empty stop strings — drives the stop-string match branch in step_token.
    {
        entropic::GenerationParams params;
        params.max_tokens = 4;
        params.temperature = 0.0f;
        params.enable_thinking = false;
        params.stop = {"\n", "stop"};
        auto r = backend.complete("hi", params);
        (void)r;
    }

    // state save/load round-trip on a non-default seq id.
    {
        std::vector<uint8_t> buf;
        bool saved = backend.save_state(1, buf);
        if (saved && !buf.empty()) {
            (void)backend.clear_state(1);
            (void)backend.restore_state(1, buf);
        }
    }

    // Prompt cache + state-clear
    backend.clear_prompt_cache();
    (void)backend.clear_state(-1);

    // Deactivate
    backend.deactivate();
    REQUIRE(backend.state() == entropic::ModelState::WARM);
    REQUIRE_FALSE(backend.is_active());
    REQUIRE(backend.is_loaded());

    // Unload (and idempotency)
    backend.unload();
    REQUIRE(backend.state() == entropic::ModelState::COLD);
    REQUIRE_FALSE(backend.is_loaded());
    backend.unload();  // idempotent
    REQUIRE(backend.state() == entropic::ModelState::COLD);
}

// Note: a second TEST_CASE that re-loads the model has been
// intentionally omitted. Repeated load/unload cycles within the same
// process hit a SEGV in llama.cpp's CUDA shutdown path on dev builds
// (existing multi-instance issue tracked in v2.2.x bug reports). The
// single-load test above exercises every load+activate→generate path
// needed for coverage.
