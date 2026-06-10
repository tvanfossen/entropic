// SPDX-License-Identifier: Apache-2.0
/**
 * @file llama_cpp_backend.cpp
 * @brief LlamaCppBackend implementation — direct llama.cpp C API.
 *
 * Pinned against llama.cpp submodule b8420. Uses:
 * - llama_model_load_from_file() for model loading
 * - llama_init_from_model() for context creation
 * - llama_decode() + llama_batch for token processing
 * - llama_sampler_chain for sampling
 * - llama_chat_apply_template() for chat formatting
 *
 * @version 1.8.3
 */

#include "llama_cpp_backend.h"
#include "llama_cpp_sampler.h"
#include "llama_cpp_tokenizer.h"
#include "warm_keep_util.h"  // gh#96: common_prefix_len / warm_keep_cut
#include "tool_call_markers.h"  // gh#103: family-aware tool-call close marker
#include "batch_util.h"  // gh#98: batch_shared_prefix_len / batch_is_viable

#include <entropic/inference/adapters/adapter_base.h>  // gh#90 coerce_string_typed_args
#include <entropic/types/logging.h>

#include <common.h>
#include <chat.h>
#include <sampling.h>
#include <speculative.h>
#include <mtmd.h>
#include <mtmd-helper.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace entropic {

namespace {

auto logger = entropic::log::get("inference.llama_cpp");

/**
 * @brief Check if generated text ends with any stop sequence.
 * @param text Generated text so far.
 * @param stop_sequences Stop sequence list.
 * @return true if any stop sequence found at end of text.
 * @utility
 * @version 1.8.2
 */
bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size()
        && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/**
 * @brief Check if generated text ends with any stop sequence.
 * @param text Generated text so far.
 * @param stop_sequences List of stop sequences to check.
 * @return true if any stop sequence found at end of text.
 * @utility
 * @version 1.8.2
 */
bool check_stop_sequences(
    const std::string& text,
    const std::vector<std::string>& stop_sequences)
{
    for (const auto& stop : stop_sequences) {
        if (!stop.empty() && ends_with(text, stop)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Create a prefill-failed GenerationResult.
 * @return GenerationResult with error fields populated.
 * @utility
 * @version 1.10.4
 */
GenerationResult prefill_error() {
    GenerationResult r;
    r.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
    r.error_message = "Prefill decode failed";
    r.finish_reason = "error";
    return r;
}

/**
 * @brief Log sampler chain configuration at INFO.
 * @param params Generation parameters.
 * @utility
 * @version 1.10.4
 */
void log_sampler_config(const GenerationParams& params) {
    logger->info("Sampler: temp={:.2f}, top_k={}, top_p={:.2f}, "
                 "repeat_penalty={:.2f}, thinking={}",
                 params.temperature, params.top_k, params.top_p,
                 params.repeat_penalty, params.enable_thinking);
}

/**
 * @brief Populate timing fields and log post-generation summary.
 * @param result Generation result (mutated: timing fields populated).
 * @param start_time Generation start time.
 * @utility
 * @version 1.10.4
 */
void finalize_result(GenerationResult& result,
    std::chrono::steady_clock::time_point start_time)
{
    auto end = entropic::log::now();
    result.generation_time_ms = entropic::log::elapsed_ms(
        start_time, end);
    if (result.token_count > 0 && result.generation_time_ms > 0.0) {
        result.throughput_tok_s =
            static_cast<double>(result.token_count)
            / result.generation_time_ms * 1000.0;
    }
    logger->info("Generated: {} tokens, finish={}, {:.0f}ms, "
                 "{:.1f} tok/s",
                 result.token_count, result.finish_reason,
                 result.generation_time_ms, result.throughput_tok_s);
    logger->info("Content:\n{}", result.content);
}

/**
 * @brief Apply the terminal length-finish + content/token-count fields and
 *        finalize timing on a completed text-generation result.
 *
 * Shared tail of every text decode loop (run_sampling_loop, both
 * do_generate_text_only variants, do_generate_streaming_text_only). Extracted
 * (DRY, v2.8.3) so each loop body stays within the knots ABC budget and the
 * "ran to the token cap => finish_reason=length" convention lives in one place.
 *
 * @param result      Result accumulated by the loop. finish_reason is set by
 *                    the loop on EOS/stop/error/cancel; empty here means the
 *                    loop ran to the token cap (=> "length").
 * @param generated   Accumulated decoded text.
 * @param n_generated Tokens generated.
 * @param params      Generation params (max_tokens for the length check).
 * @param t0          Loop start time, passed through to finalize_result.
 * @utility
 * @version 2.8.3
 */
void finalize_generation(GenerationResult& result,
    const std::string& generated, int n_generated,
    const GenerationParams& params,
    std::chrono::steady_clock::time_point t0)
{
    if (n_generated >= params.max_tokens
            && result.finish_reason.empty()) {
        result.finish_reason = "length";
    }
    result.content = generated;
    result.token_count = n_generated;
    finalize_result(result, t0);
}

/**
 * @brief Build a fully-finalized GenerationResult for sampler-init failure.
 *
 * Extracted in v2.3.10 to keep do_generate_text_only and
 * do_generate_streaming_text_only within the knots SLOC + ABC budgets
 * after the sampler-seam refactor added the null-sampler check.
 * @param t0 Generation start time, passed through to finalize_result.
 * @return GenerationResult with error_code=GENERATE_FAILED, error_message
 *         set, finish_reason="error", and timing fields populated.
 * @utility
 * @version 2.3.10
 */
GenerationResult sampler_init_error(
    std::chrono::steady_clock::time_point t0)
{
    GenerationResult r;
    r.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
    r.error_message = "Sampler factory not initialized";
    r.finish_reason = "error";
    finalize_result(r, t0);
    return r;
}

/**
 * @brief Map a ModelConfig::cache_type_k/v string to a ggml_type.
 *
 * Unknown values fall back to F16 with a logged warning, matching the
 * llama_context_default_params() default.
 *
 * @utility
 * @version 2.2.9
 */
ggml_type parse_kv_cache_type(const std::string& s) {
    static const std::pair<const char*, ggml_type> kTable[] = {
        {"f16",  GGML_TYPE_F16},
        {"f32",  GGML_TYPE_F32},
        {"bf16", GGML_TYPE_BF16},
        {"q8_0", GGML_TYPE_Q8_0},
        {"q4_0", GGML_TYPE_Q4_0},
    };
    for (const auto& [name, type] : kTable) {
        if (s == name) { return type; }
    }
    logger->warn("Unknown cache_type '{}' — defaulting to f16", s);
    return GGML_TYPE_F16;
}

/**
 * @brief Map a ModelConfig::split_mode string to llama_split_mode (gh#23 item 6).
 *
 * Empty string → llama.cpp's default (`LAYER`). Unknown values fall
 * back to default with a logged warning.
 *
 * @utility
 * @version 2.3.18
 */
llama_split_mode parse_split_mode(const std::string& s) {
    if (s.empty()) { return LLAMA_SPLIT_MODE_LAYER; }
    static const std::pair<const char*, llama_split_mode> kTable[] = {
        {"none",  LLAMA_SPLIT_MODE_NONE},
        {"layer", LLAMA_SPLIT_MODE_LAYER},
        {"row",   LLAMA_SPLIT_MODE_ROW},
    };
    for (const auto& [name, mode] : kTable) {
        if (s == name) { return mode; }
    }
    logger->warn("Unknown split_mode '{}' — defaulting to layer", s);
    return LLAMA_SPLIT_MODE_LAYER;
}

/**
 * @brief Build llama_model_params for GPU model load.
 *
 * Extracted (gh#23 v2.3.18) to keep `load_gpu_model` under the knots
 * ABC gate as new MVP-10 model-load knobs land (`split_mode`,
 * `main_gpu`, `offload_kqv`, `rope_freq_*`).
 *
 * @utility
 * @version 2.3.18
 */
llama_model_params build_load_mparams(const entropic::ModelConfig& cfg) {
    llama_model_params m = llama_model_default_params();
    m.n_gpu_layers = cfg.gpu_layers;
    m.use_mmap = true;
    m.use_mlock = cfg.use_mlock;
    m.split_mode = parse_split_mode(cfg.split_mode);
    // gh#23 MVP item 7 (v2.3.19): main_gpu. Effective when split_mode
    // is "none" (pin) or "row" (small-tensor placement). 0 keeps
    // pre-v2.3.19 load bit-for-bit.
    m.main_gpu = cfg.main_gpu;
    return m;
}

} // anonymous namespace

// ── Lifecycle ──────────────────────────────────────────────

/**
 * @brief Load model into CPU RAM (COLD → WARM).
 *
 * Uses llama_model_load_from_file with n_gpu_layers=0 for CPU-only
 * mmap+mlock loading. Model stays in page cache for fast reactivation.
 *
 * @param config Validated model config.
 * @return true on success.
 * @internal
 * @version 2.7.6
 */
bool LlamaCppBackend::do_load(const ModelConfig& config) {
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;
    mparams.use_mmap = true;
    mparams.use_mlock = config.use_mlock;

    model_ = llama_model_load_from_file(config.path.c_str(), mparams);
    if (!model_) {
        last_error_ = "llama_model_load_from_file failed: " + config.path.string();
        return false;
    }

    vocab_ = llama_model_get_vocab(model_);
    is_recurrent_ = llama_model_is_recurrent(model_);
    is_hybrid_ = llama_model_is_hybrid(model_);  // gh#97: attn + recurrent/SSM
    // v2.3.10: wire the Tokenizer seam now that vocab_ is valid.
    // Lifetime: tokenizer_ borrows vocab_; do_unload resets
    // tokenizer_ BEFORE freeing the model so the borrow never dangles.
    tokenizer_ = std::make_unique<LlamaCppTokenizer>(vocab_);
    logger->info("Model loaded (CPU): {} tokens in vocab, recurrent={}",
              llama_vocab_n_tokens(vocab_), is_recurrent_);
    return true;
}

/**
 * @brief Activate model on GPU (WARM → ACTIVE).
 *
 * Reloads model with n_gpu_layers from config, then creates
 * inference context with KV cache.
 *
 * @return true on success.
 * @internal
 * @version 2.1.8
 */
namespace {
/**
 * @brief Build llama_context_params from a ModelConfig.
 *
 * Extracted from `do_activate` in v2.2.9 to keep SLOC under the knots
 * gate after v2.2.7 wired cache_type_k/v + the v2.2.8 diagnostic
 * expansion. gh#61: maps ModelConfig::cache_type_k/cache_type_v
 * (documented since v1.8.0, dead-code until v2.2.7) into the actual
 * llama.cpp KV-cache quantization slots. gh#98 (v2.8.0): kv_unified when
 * n_parallel>1 so the same-prefix batch fan-out's seq_cp is supported.
 *
 * @utility
 * @version 2.8.0
 */
llama_context_params build_cparams(const entropic::ModelConfig& cfg) {
    llama_context_params c = llama_context_default_params();
    c.n_ctx = static_cast<uint32_t>(cfg.context_length);
    c.n_batch = static_cast<uint32_t>(cfg.n_batch);
    // gh#23 MVP item 5 (v2.3.17): n_ubatch. 0 keeps llama.cpp's default
    // (== n_batch in practice), preserving pre-v2.3.17 chunking.
    if (cfg.n_ubatch > 0) {
        c.n_ubatch = static_cast<uint32_t>(cfg.n_ubatch);
    }
    c.n_threads = cfg.n_threads > 0
        ? static_cast<uint32_t>(cfg.n_threads)
        : std::thread::hardware_concurrency();
    c.flash_attn_type = cfg.flash_attn
        ? LLAMA_FLASH_ATTN_TYPE_ENABLED
        : LLAMA_FLASH_ATTN_TYPE_DISABLED;
    c.type_k = parse_kv_cache_type(cfg.cache_type_k);
    c.type_v = parse_kv_cache_type(cfg.cache_type_v);
    // gh#23 MVP item 8 (v2.3.20): offload_kqv. true (default) matches
    // llama.cpp's default — bit-identical for callers not opting out.
    c.offload_kqv = cfg.offload_kqv;
    // gh#23 MVP items 9 + 10 (v2.3.21 + v2.3.22): RoPE frequency
    // overrides. Both 0.0 = use model's trained value — bit-identical.
    c.rope_freq_base = cfg.rope_freq_base;
    c.rope_freq_scale = cfg.rope_freq_scale;
    // gh#23 MVP item 11 (v2.3.23): n_parallel maps to cparams.n_seq_max.
    // 1 (default) matches llama.cpp's default — bit-identical.
    c.n_seq_max = static_cast<uint32_t>(cfg.n_parallel);
    // gh#98 (v2.8.0): a unified KV buffer is REQUIRED for llama_memory_seq_cp
    // (the same-prefix batch fan-out) — seq_cp asserts on per-sequence buffers.
    // llama.cpp also recommends kv_unified exactly when sequences share a large
    // prefix (our case). Only enabled when batching is configured (n_parallel>1)
    // so single-sequence handles keep llama.cpp's default.
    c.kv_unified = (cfg.n_parallel > 1);
    return c;
}
} // anonymous namespace

/**
 * @brief Activate model on GPU (WARM → ACTIVE).
 *
 * Reloads model with n_gpu_layers from config, then creates inference
 * context with KV cache. v2.2.7 (gh#61) wired cache_type_k/v.
 * v2.2.8 (gh#58 follow-up) added the diagnostic-rich error message.
 * v2.2.9 extracted the cparams builder to satisfy the SLOC gate.
 *
 * @return true on success.
 * @internal
 * @version 2.3.7
 */
bool LlamaCppBackend::do_activate() {
    if (!load_gpu_model()) { return false; }
    if (!create_inference_context()) { return false; }
    // v2.3.10: wire the Sampler seam once ctx_ / vocab_ are live.
    // Lifetime: factory borrows ctx_ + vocab_; do_deactivate /
    // do_unload reset sampler_factory_ BEFORE freeing those handles
    // so the borrow never dangles.
    sampler_factory_ = std::make_unique<LlamaCppSamplerFactory>(
        ctx_, vocab_);
    init_mmproj_if_configured();
    return true;
}

/**
 * @brief Load the GGUF model onto the GPU (do_activate step 1).
 * @return true on success; sets last_error_ on failure.
 *
 * gh#87 (v2.7.0): frees the WARM CPU model BEFORE reloading with GPU
 * layers. The prior order (load new_model, *then* free the old model_)
 * held two full llama_model objects resident during a single activate —
 * a transient double-load. Changing n_gpu_layers requires a reload (you
 * cannot re-offload an already-loaded model), but there is no reason to
 * keep the discarded CPU copy alive across it: a failed GPU reload
 * leaves the backend unusable regardless, and the COLD path can recover
 * it. Freeing first removes the simultaneity (and the duplicate model
 * metadata/buffers) without changing the load contract.
 *
 * @internal
 * @version 2.7.0
 */
bool LlamaCppBackend::load_gpu_model() {
    llama_model_params mparams = build_load_mparams(config());

    if (!config().tensor_split.empty()) {
        // TODO: parse tensor_split string into float array for multi-GPU
        logger->warn("tensor_split not yet implemented, ignoring");
    }

    // tokenizer_ borrows the old vocab_; reset it before the free so the
    // borrow never dangles. Then free the WARM model and null the
    // handles so a failed reload below leaves the backend in a clean,
    // recoverable state rather than a dangling one.
    tokenizer_.reset();
    if (model_ != nullptr) {
        llama_model_free(model_);
        model_ = nullptr;
        vocab_ = nullptr;
    }

    model_ = llama_model_load_from_file(config().path.c_str(), mparams);
    if (model_ == nullptr) {
        // llama.cpp returns null with no error string — the actual
        // reason (OOM, CUDA init failure, GGUF parse error, etc.) only
        // surfaces in ggml's log stream. Point the operator at it so
        // multi-handle GPU failures (gh#58 v2.2.7 follow-up) are
        // diagnosable without source-diving llama.cpp.
        last_error_ = "Failed to reload model with GPU layers "
                      "(path=" + config().path.string()
                    + ", gpu_layers=" + std::to_string(config().gpu_layers)
                    + ") — check llama_ggml.log in the engine's log_dir "
                      "for the underlying llama.cpp/CUDA error";
        return false;
    }

    vocab_ = llama_model_get_vocab(model_);
    tokenizer_ = std::make_unique<LlamaCppTokenizer>(vocab_);
    return true;
}

/**
 * @brief Create the llama context + prompt cache (do_activate step 2).
 * @return true on success; sets last_error_ on failure.
 * @internal
 * @version 2.3.7
 */
bool LlamaCppBackend::create_inference_context() {
    llama_context_params cparams = build_cparams(config());

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_) {
        last_error_ = "llama_init_from_model failed";
        return false;
    }

    logger->info("Context created: n_ctx={}, n_batch={}, "
                 "flash_attn={}, type_k={}, type_v={}",
                 config().context_length, config().n_batch,
                 config().flash_attn,
                 config().cache_type_k, config().cache_type_v);

    // Initialize prompt cache if not already created
    if (!prompt_cache_) {
        prompt_cache_ = std::make_unique<PromptCache>(
            prompt_cache_config_.max_bytes);
        logger->info("Prompt cache initialized: max_bytes={}",
                     prompt_cache_config_.max_bytes);
    }
    return true;
}

/**
 * @brief Initialize libmtmd context if mmproj is configured (v2.1.8).
 *
 * Extracted from do_activate to keep that function under the knots
 * SLOC threshold. mtmd holds a reference to the live `model_`, so
 * init runs after the GPU reload and before any generation. Failure
 * is non-fatal — the engine falls back to text-only with a logged
 * diagnostic.
 *
 * @internal
 * @version 2.1.8
 */
void LlamaCppBackend::init_mmproj_if_configured() {
    if (config().mmproj_path.empty()) {
        has_vision_ = false;
        return;
    }
    auto ctx_params = mtmd_context_params_default();
    ctx_params.use_gpu = (config().gpu_layers != 0);
    ctx_params.flash_attn_type = config().flash_attn
        ? LLAMA_FLASH_ATTN_TYPE_ENABLED
        : LLAMA_FLASH_ATTN_TYPE_DISABLED;
    ctx_params.print_timings = false;
    mtmd_ctx_ = mtmd_init_from_file(
        config().mmproj_path.c_str(), model_, ctx_params);
    if (mtmd_ctx_ == nullptr) {
        logger->error("mtmd_init_from_file failed for {} — "
                      "continuing in text-only mode",
                      config().mmproj_path.string());
        has_vision_ = false;
        return;
    }
    has_vision_ = mtmd_support_vision(mtmd_ctx_);
    logger->info("mmproj loaded from {} — vision={}",
                 config().mmproj_path.string(), has_vision_);
}

/**
 * @brief Deactivate: free context, reload model CPU-only.
 *
 * gh#87 (v2.7.0): frees the GPU model BEFORE reloading CPU-only, the
 * symmetric fix to load_gpu_model. Deactivate's whole purpose is to
 * release VRAM and stay WARM, so freeing the GPU model first aligns
 * with intent and avoids holding two full llama_model objects during
 * the tier-switch reload. The old "keep GPU model on CPU-reload
 * failure" fallback is dropped — it left VRAM pinned, contradicting the
 * deactivation; a failed warm-reload now nulls the handle (recoverable
 * via the next activate, which reloads from scratch).
 *
 * @internal
 * @version 2.7.5
 */
void LlamaCppBackend::do_deactivate() {
    // v2.3.10: sampler factory borrows ctx_ + vocab_. Release it
    // BEFORE freeing the context so the borrow never dangles.
    sampler_factory_.reset();
    // v2.1.8: mtmd holds a reference to the live llama_model — free
    // it before the GPU model is unloaded below.
    if (mtmd_ctx_ != nullptr) {
        mtmd_free(mtmd_ctx_);
        mtmd_ctx_ = nullptr;
        has_vision_ = false;
    }
    if (ctx_) {
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    invalidate_resident_kv();  // gh#96: KV is gone with the context

    // Free the GPU model FIRST (releasing VRAM — the point of
    // deactivate), then reload CPU-only for the WARM state. tokenizer_
    // borrows the old vocab_, so reset it before the free.
    tokenizer_.reset();
    if (model_ != nullptr) {
        llama_model_free(model_);
        model_ = nullptr;
        vocab_ = nullptr;
    }

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;
    mparams.use_mmap = true;
    mparams.use_mlock = config().use_mlock;

    model_ = llama_model_load_from_file(config().path.c_str(), mparams);
    if (model_ != nullptr) {
        vocab_ = llama_model_get_vocab(model_);
        tokenizer_ = std::make_unique<LlamaCppTokenizer>(vocab_);
    } else {
        // VRAM is released, but the warm-reload failed: leave the handle
        // null (state stays recoverable — the next activate reloads from
        // scratch). Error, not warn: a same-file CPU reload failing here
        // signals real trouble (disk/OOM).
        logger->error("Failed to reload CPU model during deactivate "
                      "(path={}); backend left unloaded until next activate",
                      config().path.string());
    }
}

/**
 * @brief Destructor — route to do_unload() so GPU buffers don't leak.
 *
 * gh#58 v2.2.7 follow-up: previously the defaulted base destructor
 * left model_/ctx_/mtmd_ctx_ as raw pointers that were never freed.
 * On a second handle's GPU model load, llama.cpp's CUDA pool then
 * failed because the prior buffers were still allocated.
 *
 * @internal
 * @version 2.2.8
 */
LlamaCppBackend::~LlamaCppBackend() {
    do_unload();
}

/**
 * @brief Wire a mock Tokenizer for unit tests + mark the backend
 *        as WARM so is_loaded()-gated public methods route to it.
 *
 * Implementation note: state_ is set directly to WARM here rather
 * than via the normal load() path because there's no underlying
 * model. The destructor's do_unload() handles cleanup — the
 * tokenizer_.reset() happens BEFORE the (nullptr) model_/vocab_
 * are touched, so no dangling-borrow risk.
 *
 * @internal
 * @version 2.3.10
 */
void LlamaCppBackend::inject_tokenizer_for_test(
    std::unique_ptr<Tokenizer> tokenizer)
{
    tokenizer_ = std::move(tokenizer);
    state_.store(ModelState::WARM, std::memory_order_release);
}

/**
 * @brief Wire a mock SamplerFactory for unit tests (v2.3.10 seam).
 *
 * Sister hook to `inject_tokenizer_for_test`. Does NOT touch
 * `state_` — the decode loop's `is_loaded()`-gated entry points
 * are already covered by the tokenizer seam, and Sampler tests
 * exercise the factory at a finer grain than full backend
 * lifecycle. Tests that need a "WARM" backend can chain both
 * inject_*_for_test calls (tokenizer flips state, sampler
 * factory then plugs in without disturbing it).
 *
 * @internal
 * @version 2.3.10
 */
void LlamaCppBackend::inject_sampler_factory_for_test(
    std::unique_ptr<SamplerFactory> factory)
{
    sampler_factory_ = std::move(factory);
}

/**
 * @brief Full unload — free all resources, clear prompt cache.
 * @internal
 * @version 2.7.5
 */
void LlamaCppBackend::do_unload() {
    if (prompt_cache_) {
        prompt_cache_->clear();
    }
    // v2.3.10: sampler factory borrows ctx_ + vocab_ — release it
    // BEFORE the context/model are freed below so the borrow never
    // points into freed memory. (do_deactivate normally releases
    // this earlier; this reset is the WARM→COLD safety net.)
    sampler_factory_.reset();
    // v2.3.10: tokenizer borrows vocab_ — release it BEFORE the model
    // is freed so the borrow never points into freed memory.
    tokenizer_.reset();
    // v2.1.8: mtmd must be freed before the underlying llama_model.
    if (mtmd_ctx_ != nullptr) {
        mtmd_free(mtmd_ctx_);
        mtmd_ctx_ = nullptr;
        has_vision_ = false;
    }
    if (ctx_) {
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    invalidate_resident_kv();  // gh#96: KV is gone with the context
    if (model_) {
        llama_model_free(model_);
        model_ = nullptr;
    }
    vocab_ = nullptr;
}

// ── Tokenization ───────────────────────────────────────────

/**
 * @brief Tokenize text using model vocabulary.
 * @param text Input text.
 * @param add_special Add BOS/EOS special tokens.
 * @return Vector of token IDs.
 * @internal
 * @version 1.8.2
 */
std::vector<llama_token> LlamaCppBackend::tokenize(
    const std::string& text, bool add_special) const
{
    // v2.3.10: route through the Tokenizer seam. tokenizer_ is set
    // in do_load (real impl) or via inject_tokenizer_for_test (mock).
    // Returns empty when no tokenizer is wired — matches the prior
    // failure-path return shape.
    if (!tokenizer_) { return {}; }
    auto ids = tokenizer_->tokenize(text, add_special);
    // llama_token is int32_t; vector conversion is a copy through
    // iterators since the value type matches.
    return {ids.begin(), ids.end()};
}

/**
 * @brief Detokenize a single token to string.
 * @param token Token ID.
 * @return String representation.
 * @internal
 * @version 1.8.2
 */
std::string LlamaCppBackend::detokenize(llama_token token) const {
    // v2.3.10: route through Tokenizer seam. The special=false /
    // gh#68 history + defensive rationale now lives in
    // LlamaCppTokenizer::detokenize. Returns empty when no
    // tokenizer is wired — matches prior failure-path return.
    if (!tokenizer_) { return {}; }
    return tokenizer_->detokenize(static_cast<int32_t>(token));
}

/**
 * @brief Count tokens in text.
 * @param text Input text.
 * @return Token count.
 * @internal
 * @version 1.8.2
 */
int LlamaCppBackend::do_count_tokens(const std::string& text) const {
    auto tokens = tokenize(text, false);
    return static_cast<int>(tokens.size());
}

/**
 * @brief Tokenize text to token IDs using model vocabulary.
 * @param text Input text.
 * @return Token ID vector with BOS.
 * @utility
 * @version 1.10.2
 */
std::vector<int32_t> LlamaCppBackend::tokenize_text(
    const std::string& text) const {
    auto tokens = tokenize(text, true);
    return {tokens.begin(), tokens.end()};
}

// ── Evaluation (v1.9.10) ──────────────────────────────────

/**
 * @brief Evaluate per-token log-probabilities via sequential decode.
 *
 * Clears memory, then processes tokens one at a time using the same
 * decode path as generation. After each token, extracts logits for
 * the next-token prediction. Compatible with recurrent/hybrid models
 * that only support single-output-position batches.
 *
 * @param tokens Token IDs to evaluate.
 * @param n_tokens Number of tokens (minimum 2).
 * @return LogprobResult with per-transition logprobs and perplexity.
 * @internal
 * @version 1.10.2
 */
LogprobResult LlamaCppBackend::do_evaluate_logprobs(
    const int32_t* tokens,
    int n_tokens)
{
    int n_vocab = llama_vocab_n_tokens(vocab_);
    LogprobResult result;
    result.tokens.assign(tokens, tokens + n_tokens);
    result.n_tokens = n_tokens;
    result.n_logprobs = n_tokens - 1;
    result.logprobs.reserve(result.n_logprobs);

    auto* mem = llama_get_memory(ctx_);
    llama_memory_clear(mem, true);

    for (int i = 0; i < n_tokens; i++) {
        llama_token tok = tokens[i];
        llama_batch batch = llama_batch_get_one(&tok, 1);
        int rc = llama_decode(ctx_, batch);
        if (rc != 0) {
            llama_memory_clear(mem, true);
            throw std::runtime_error("llama_decode failed at logprob pos");
        }
        if (i < n_tokens - 1) {
            const float* logits = llama_get_logits_ith(ctx_, -1);
            float lp = extract_token_logprob(
                logits, tokens[i + 1], n_vocab);
            result.logprobs.push_back(lp);
        }
    }

    float sum = 0.0f;
    for (float lp : result.logprobs) { sum += lp; }
    result.total_logprob = sum;
    result.perplexity = std::exp(
        -sum / static_cast<float>(result.n_logprobs));

    llama_memory_clear(mem, true);
    return result;
}

/**
 * @brief Allocate a temporary sequence ID for evaluation.
 *
 * Reuses a released id from the pool, else mints a fresh monotonic id.
 * gh#98 (v2.8.0): the old `1 + free_seq_ids_.size()` returned 1 on EVERY
 * empty-pool call, so a multi-seq batch (which allocates N ids back-to-back
 * with no intervening release) got duplicate ids → seq_id collision →
 * shared KV slots. The monotonic counter mints distinct ids; released ids
 * still return to the pool and are reused first, so it stays bounded by
 * concurrent demand (≤ n_parallel for a valid batch).
 *
 * @return Unused seq_id (starts at 1, 0 is generation).
 * @internal
 * @version 2.8.0
 */
llama_seq_id LlamaCppBackend::allocate_temp_seq_id() {
    std::lock_guard<std::mutex> lock(seq_id_mutex_);
    if (!free_seq_ids_.empty()) {
        auto id = free_seq_ids_.back();
        free_seq_ids_.pop_back();
        return id;
    }
    return next_temp_seq_id_++;
}

/**
 * @brief Release a temporary sequence ID back to the pool.
 * @param seq_id The seq_id to release.
 * @internal
 * @version 1.10.2
 */
void LlamaCppBackend::release_temp_seq_id(llama_seq_id seq_id) {
    std::lock_guard<std::mutex> lock(seq_id_mutex_);
    free_seq_ids_.push_back(seq_id);
}

/**
 * @brief Extract log-probability for a token from logits.
 *
 * Uses numerically stable log-softmax:
 *   log P(t) = logits[t] - max - log(sum(exp(logits - max)))
 *
 * @param logits Raw logits array.
 * @param next_token Token to score.
 * @param n_vocab Vocabulary size.
 * @return log P(next_token | context).
 * @internal
 * @version 1.9.10
 */
float LlamaCppBackend::extract_token_logprob(
    const float* logits,
    int32_t next_token,
    int n_vocab)
{
    float max_logit = logits[0];
    for (int v = 1; v < n_vocab; v++) {
        if (logits[v] > max_logit) {
            max_logit = logits[v];
        }
    }
    float sum_exp = 0.0f;
    for (int v = 0; v < n_vocab; v++) {
        sum_exp += std::exp(logits[v] - max_logit);
    }
    float log_sum_exp = max_logit + std::log(sum_exp);
    return logits[next_token] - log_sum_exp;
}

// ── Chat template ──────────────────────────────────────────

/**
 * @brief Convert engine messages to llama_chat_message views.
 * @param messages Source messages (must outlive the returned views).
 * @return Vector of {role, content} c_str pointers into `messages`.
 * @utility
 * @version 2.3.7
 */
static std::vector<llama_chat_message> to_llama_chat(
    const std::vector<Message>& messages) {
    std::vector<llama_chat_message> chat_msgs;
    chat_msgs.reserve(messages.size());
    for (const auto& msg : messages) {
        chat_msgs.push_back({msg.role.c_str(), msg.content.c_str()});
    }
    return chat_msgs;
}

/**
 * @brief Convert engine messages to common_chat_msg (gh#86, v2.6.1).
 *
 * Role + content only. The legacy `apply_chat_template` path renders
 * without tools (entropic injects tool prompts into the system message via
 * ResponseGenerator::inject_tool_prompt); the gh#87 `render_with_tools`
 * path supplies tools through `inputs.tools` instead.
 *
 * @param messages Source messages.
 * @return Vector of common_chat_msg.
 * @utility
 * @version 2.6.1
 */
static std::vector<common_chat_msg> to_common_chat(
    const std::vector<Message>& messages) {
    std::vector<common_chat_msg> out;
    out.reserve(messages.size());
    for (const auto& msg : messages) {
        common_chat_msg cm;
        cm.role = msg.role;
        cm.content = msg.content;
        out.push_back(std::move(cm));
    }
    return out;
}

/**
 * @brief Convert entropic MCP tool JSON to common_chat_tool defs (gh#87).
 *
 * Reads `ServerManager::list_tools()` shape — an array of
 * `{name, description, inputSchema}` — into `common_chat_tool`
 * (name + description + parameters-schema-string). Tools without a name
 * are skipped. Malformed JSON yields an empty list (the render then
 * proceeds tool-less rather than throwing).
 *
 * @param tools_json MCP tool-list JSON array string.
 * @return Parsed common_chat tool definitions.
 * @utility
 * @version 2.7.0
 */
static std::vector<common_chat_tool> mcp_tools_to_common_chat(
    const std::string& tools_json) {
    std::vector<common_chat_tool> out;
    if (tools_json.empty()) { return out; }
    auto arr = nlohmann::json::parse(tools_json, nullptr, false);
    if (!arr.is_array()) { return out; }
    for (const auto& t : arr) {
        common_chat_tool ct;
        ct.name = t.value("name", "");
        ct.description = t.value("description", "");
        if (t.contains("inputSchema")) {
            ct.parameters = t["inputSchema"].dump();
        }
        if (!ct.name.empty()) { out.push_back(std::move(ct)); }
    }
    return out;
}

/**
 * @brief Map a common_chat_tool_call to entropic's ToolCall (gh#87).
 *
 * Name + id pass through; the JSON arguments string is kept verbatim in
 * `arguments_json` (for passthrough dispatch) and also flattened into the
 * string key-value `arguments` map (object values dumped back to JSON).
 *
 * @param cc Native common_chat tool call.
 * @return entropic ToolCall.
 * @utility
 * @version 2.7.0
 */
static ToolCall to_entropic_tool_call(const common_chat_tool_call& cc) {
    ToolCall tc;
    tc.id = cc.id;
    tc.name = cc.name;
    tc.arguments_json = cc.arguments;
    auto j = nlohmann::json::parse(cc.arguments, nullptr, false);
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            tc.arguments[it.key()] =
                it->is_string() ? it->get<std::string>() : it->dump();
        }
    }
    return tc;
}

/**
 * @brief Shared common_chat render core for both template paths (gh#87).
 *
 * Builds `common_chat_templates_inputs` (jinja, generation prompt,
 * enable_thinking, optional tools) and applies the model's template.
 * Returns the full `common_chat_params` so callers can use `.prompt`
 * and — for the tools path — capture `.format`/`.generation_prompt`/
 * `.parser` for a later parse. Returns nullopt when the model isn't
 * loaded, the template can't init, or apply throws (caller falls back
 * to the low-level path).
 *
 * @param model Loaded model (may be null → nullopt).
 * @param messages Conversation history.
 * @param params Generation parameters (enable_thinking honored).
 * @param tools Tool defs for `inputs.tools` (empty for the legacy path).
 * @return Rendered params, or nullopt on any failure.
 * @utility
 * @version 2.7.0
 */
static std::optional<common_chat_params> render_common_chat(
    llama_model* model,
    const std::vector<Message>& messages,
    const GenerationParams& params,
    const std::vector<common_chat_tool>& tools) {
    if (model == nullptr) { return std::nullopt; }
    auto tmpls = common_chat_templates_init(model, "");
    std::optional<common_chat_params> out;
    if (tmpls) {
        common_chat_templates_inputs inputs;
        inputs.messages = to_common_chat(messages);
        inputs.add_generation_prompt = true;
        inputs.use_jinja = true;
        inputs.enable_thinking = params.enable_thinking;  // gh#86
        inputs.tools = tools;
        if (!tools.empty()) {
            inputs.tool_choice = COMMON_CHAT_TOOL_CHOICE_AUTO;
        }
        try {
            out = common_chat_templates_apply(tmpls.get(), inputs);
        } catch (const std::exception& e) {
            logger->warn("jinja chat template apply failed ({}); "
                         "falling back to low-level template", e.what());
        }
    }
    return out;
}

/**
 * @brief Plain "role: content" join used when templating fails.
 * @param messages Conversation history.
 * @return Concatenated fallback prompt.
 * @utility
 * @version 2.3.7
 */
static std::string concat_messages_fallback(
    const std::vector<Message>& messages) {
    std::string fallback;
    for (const auto& msg : messages) {
        fallback += msg.role + ": " + msg.content + "\n";
    }
    return fallback;
}

/**
 * @brief Render the GGUF chat template for messages (gh#86/gh#87).
 *
 * The legacy (tool-less) render path: delegates to the shared
 * `render_common_chat` core with no tools, so the template's
 * `enable_thinking` variable receives `params.enable_thinking` (the
 * low-level `llama_chat_apply_template()` had no thinking slot — its bool
 * arg is `add_generation_prompt` — so a tier's `enable_thinking: false`
 * was silently dropped pre-v2.6.1). Tools are intentionally absent here;
 * the gh#87 `render_with_tools` path supplies them via `inputs.tools`.
 *
 * Falls back to the low-level template (then a plain join) if the jinja
 * path is unavailable (e.g. a GGUF with no embedded template).
 *
 * @param messages Conversation history.
 * @param params Generation parameters (enable_thinking honored).
 * @return Formatted prompt string.
 * @internal
 * @version 2.7.0
 */
std::string LlamaCppBackend::apply_chat_template(
    const std::vector<Message>& messages,
    const GenerationParams& params) const
{
    auto rendered = render_common_chat(model_, messages, params, {});
    return rendered ? rendered->prompt
                    : apply_chat_template_lowlevel(messages);
}

/**
 * @brief Generation render seam (gh#87): tools → common_chat, else legacy.
 *
 * Clears any stale captured params on the tool-less branch so
 * has_common_chat_params() reflects only THIS render. See header.
 *
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @return Formatted prompt string.
 * @internal
 * @version 2.7.0
 */
std::string LlamaCppBackend::render_prompt(
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    if (!active_tools_json_.empty()) {
        return render_with_tools(messages, params);
    }
    have_chat_params_ = false;
    return apply_chat_template(messages, params);
}

/**
 * @brief Stage tool defs for the next common_chat render (gh#87).
 * @param tools_json MCP tool-list JSON array.
 * @utility
 * @version 2.7.0
 */
void LlamaCppBackend::set_active_tools(const std::string& tools_json) {
    active_tools_json_ = tools_json;
    logger->info("Active tools staged for common_chat render: {} bytes",
                 tools_json.size());
}

/**
 * @brief Render messages through common_chat WITH active tools (gh#87).
 *
 * Captures the rendered params (format/generation_prompt/parser) so
 * parse_response can decode the emission. See header for contract.
 *
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @return Formatted prompt string.
 * @internal
 * @version 2.8.3
 */
std::string LlamaCppBackend::render_with_tools(
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    have_chat_params_ = false;
    auto tools = mcp_tools_to_common_chat(active_tools_json_);
    auto rendered = render_common_chat(model_, messages, params, tools);
    std::string prompt;
    if (rendered) {
        last_chat_format_ = static_cast<int>(rendered->format);
        last_generation_prompt_ = rendered->generation_prompt;
        last_parser_ = rendered->parser;
        have_chat_params_ = true;
        // gh#105: snapshot this TOOLED render for the engine's later re-parse.
        // A toolless interleave (validator critique) clears have_chat_params_
        // but NOT this snapshot, so parse_response still decodes the main call.
        parse_chat_format_ = last_chat_format_;
        parse_generation_prompt_ = last_generation_prompt_;
        parse_parser_ = last_parser_;
        parse_params_valid_ = true;
        prompt = rendered->prompt;
        logger->info("render_with_tools: format={}, {} tool(s), captured "
                     "parser ({} bytes)", last_chat_format_, tools.size(),
                     last_parser_.size());
    } else {
        prompt = apply_chat_template_lowlevel(messages);
    }
    return prompt;
}

/**
 * @brief True iff the captured common_chat format parses multi-param (gh#87).
 *
 * Only dedicated grammars (PEG_GEMMA4) are multi-parameter safe; the PEG
 * autoparser (PEG_NATIVE/PEG_SIMPLE) drops parameters past the first. See
 * header for the routing contract.
 *
 * @return true if parse_response is multi-parameter safe.
 * @utility
 * @version 2.8.3
 */
bool LlamaCppBackend::common_chat_parse_reliable() const {
    // gh#105: read the sticky last-TOOLED snapshot, not the live capture — the
    // engine queries this AFTER a toolless validator render would have cleared
    // have_chat_params_, so the live flag is unreliable here.
    return parse_params_valid_
        && parse_chat_format_ == COMMON_CHAT_FORMAT_PEG_GEMMA4;
}

/**
 * @brief Tool-call close marker for the captured chat format (gh#103). See
 *        the header + tool_call_markers.h. "" when no tool render captured
 *        params (no format) or the format has no confirmed per-call marker.
 * @return Close marker, or "".
 * @utility
 * @version 2.8.2
 */
std::string LlamaCppBackend::tool_call_close_marker() const {
    // last_chat_format_ is stored as int (the captured common_chat_format).
    return have_chat_params_
        ? close_marker_for_format(
              static_cast<common_chat_format>(last_chat_format_))
        : "";
}

/**
 * @brief params.stop plus the sequential close marker (gh#105). See header.
 *
 * Called post-render by each decode body so the marker reflects THIS
 * generation's captured format (the live capture). append_sequential_stop is
 * a no-op unless params.tool_call_mode == "sequential".
 * @param params Generation params.
 * @return Effective stop list.
 * @utility
 * @version 2.8.3
 */
std::vector<std::string> LlamaCppBackend::effective_stop(
    const GenerationParams& params) const {
    GenerationParams p = params;
    const std::size_t before = p.stop.size();
    append_sequential_stop(p, tool_call_close_marker());
    if (p.stop.size() > before) {
        logger->info("Sequential tier: tool-call close marker injected "
                     "post-render (gh#105) — hard-stop at first tool call");
    }
    return p.stop;
}

/**
 * @brief Parse a raw emission via the last captured render params (gh#87).
 *
 * MUST load() the serialized PEG arena — the parser_params ctor copies
 * only format + generation_prompt, so without the load the parser silently
 * degrades to pure content (Increment-1 finding). See header for contract.
 *
 * gh#90 (v2.7.2): coerces numeric scalars back to strings for string-typed
 * params (the gemma `<|"|>` escape loses type through PEG_GEMMA4).
 *
 * @param raw Raw model output (assistant turn only).
 * @return Parsed tool calls + cleaned content + reasoning.
 * @internal
 * @version 2.8.3
 */
LlamaCppBackend::CommonChatResult LlamaCppBackend::parse_response(
    const std::string& raw) const
{
    CommonChatResult result;
    // gh#105: decode from the sticky last-TOOLED snapshot (parse_*), NOT the
    // live capture — a toolless validator render between the main generation
    // and this re-parse would have cleared the live params.
    if (!parse_params_valid_) {
        result.content = raw;
        return result;
    }
    common_chat_parser_params pp;
    pp.format = static_cast<common_chat_format>(parse_chat_format_);
    pp.generation_prompt = parse_generation_prompt_;
    pp.parser.load(parse_parser_);  // mandatory — see header
    try {
        auto msg = common_chat_parse(raw, /*is_partial=*/false, pp);
        result.content = msg.content;
        result.reasoning_content = msg.reasoning_content;
        for (const auto& tc : msg.tool_calls) {
            result.tool_calls.push_back(to_entropic_tool_call(tc));
        }
        // gh#90: gemma <|"|> string-escape loses type through PEG_GEMMA4 —
        // restore string typing for params the staged schema declares string.
        coerce_string_typed_args(result.tool_calls, active_tools_json_);
    } catch (const std::exception& e) {
        logger->warn("common_chat_parse failed ({}); raw kept as content",
                     e.what());
        result.content = raw;
    }
    return result;
}

/**
 * @brief Pre-v2.6.1 low-level template path (fallback for gh#86).
 *
 * Reads the GGUF template via `llama_chat_apply_template()`. Retained
 * as the fallback when the jinja path is unavailable. Does NOT honor
 * `enable_thinking` (the low-level API has no slot for it).
 *
 * @param messages Conversation history.
 * @return Formatted prompt string, or a plain join on failure.
 * @internal
 * @version 2.6.1
 */
std::string LlamaCppBackend::apply_chat_template_lowlevel(
    const std::vector<Message>& messages) const
{
    auto chat_msgs = to_llama_chat(messages);

    int n = llama_chat_apply_template(
        nullptr, chat_msgs.data(), chat_msgs.size(),
        true, nullptr, 0);
    if (n < 0) {
        logger->error("llama_chat_apply_template failed (size query)");
        return concat_messages_fallback(messages);
    }

    std::vector<char> buf(static_cast<size_t>(n + 1));
    int written = llama_chat_apply_template(
        nullptr, chat_msgs.data(), chat_msgs.size(),
        true, buf.data(), static_cast<int32_t>(buf.size()));
    if (written < 0) {
        logger->error("llama_chat_apply_template failed (render)");
        return concat_messages_fallback(messages);
    }

    return std::string(buf.data(), static_cast<size_t>(written));
}

// ── Sampler ────────────────────────────────────────────────

/**
 * @brief Build a Sampler for one generation via the v2.3.10 seam.
 *
 * Pre-v2.3.10 this method built a `llama_sampler*` chain inline.
 * v2.3.10 moves chain construction into `LlamaCppSamplerFactory`
 * (see `llama_cpp_sampler.cpp`); this entry stays as a thin
 * wrapper so the four legacy callers (`decode_loop`,
 * `run_sampling_loop`, `do_generate_text_only`,
 * `do_generate_streaming_text_only`) remain one-liners.
 *
 * Returns nullptr when no factory has been wired — a guarded
 * fallback for the COLD-state code path that exists for
 * defensiveness (production callers only reach this from
 * ACTIVE-only code paths, where do_activate has already
 * installed the production factory).
 *
 * @param params Generation parameters.
 * @return Owned Sampler, or nullptr if no factory installed.
 * @internal
 * @version 2.3.10
 */
std::unique_ptr<Sampler> LlamaCppBackend::create_sampler(
    const GenerationParams& params) const
{
    if (!sampler_factory_) { return nullptr; }
    return sampler_factory_->create(params);
}

// ── Decode loop ────────────────────────────────────────────

/**
 * @brief Run batched prefill on input tokens.
 * @param tokens Input token sequence.
 * @return true on success.
 * @internal
 * @version 2.7.5
 */
bool LlamaCppBackend::run_prefill(const std::vector<llama_token>& tokens) {
    llama_memory_clear(llama_get_memory(ctx_), true);

    const int n_batch = config().n_batch;
    const int n_tokens = static_cast<int>(tokens.size());

    for (int i = 0; i < n_tokens; i += n_batch) {
        int chunk = std::min(n_batch, n_tokens - i);
        std::vector<llama_token> slice(
            tokens.begin() + i, tokens.begin() + i + chunk);
        llama_batch batch = llama_batch_get_one(
            slice.data(), static_cast<int32_t>(chunk));
        if (llama_decode(ctx_, batch) != 0) {
            logger->error("Prefill decode failed at offset {}", i);
            return false;
        }
    }
    last_prefill_tokens_ += n_tokens;  // gh#96: count tokens decoded in prefill
    return true;
}

/**
 * @brief Generate one token and append to output.
 *
 * v2.3.10: routes per-token draw through the Sampler seam
 * (`sampler.sample()`) instead of calling `llama_sampler_sample`
 * directly. The production LlamaCppSampler captured ctx_ at
 * construction; mocks bypass llama.cpp entirely.
 *
 * @param sampler Sampler used for the per-token draw.
 * @param generated Accumulated output string (mutated).
 * @param on_token Streaming callback (may be nullptr).
 * @param stop Stop sequences.
 * @return "continue", "stop", "eos", or "error".
 * @internal
 * @version 2.3.10
 */
std::string LlamaCppBackend::step_token(
    Sampler& sampler,
    std::string& generated,
    std::function<void(std::string_view)>& on_token,
    const std::vector<std::string>& stop)
{
    llama_token new_token = sampler.sample();

    if (new_token == llama_vocab_eos(vocab_)
        || llama_vocab_is_eog(vocab_, new_token)) {
        return "eos";
    }

    std::string piece = detokenize(new_token);
    generated += piece;
    if (on_token) {
        on_token(std::string_view(piece));
    }
    if (check_stop_sequences(generated, stop)) {
        return "stop";
    }

    llama_token tok = new_token;
    llama_batch single = llama_batch_get_one(&tok, 1);
    return (llama_decode(ctx_, single) == 0) ? "continue" : "error";
}

/**
 * @brief Core decode loop shared by generate, streaming, and complete.
 *
 * gh#98 (v2.8.0): the post-prefill sampling loop is extracted to
 * generate_after_prefill (shared with the batch path).
 *
 * @param tokens Input token sequence.
 * @param params Generation parameters.
 * @param on_token Per-token callback (nullptr for batch).
 * @param cancel Cancel flag (nullptr for batch).
 * @return GenerationResult.
 * @internal
 * @version 2.8.0
 */
GenerationResult LlamaCppBackend::decode_loop(
    const std::vector<llama_token>& tokens,
    const GenerationParams& params,
    std::function<void(std::string_view)> on_token,
    std::atomic<bool>* cancel)
{
    // v2.3.10: Sampler seam — factory installed in do_activate.
    auto sampler = create_sampler(params);
    if (!sampler) {
        GenerationResult result;
        result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        result.error_message = "Sampler factory not initialized";
        result.finish_reason = "error";
        return result;
    }

    if (!run_prefill(tokens)) {
        GenerationResult result;
        result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        result.error_message = "Prefill decode failed";
        result.finish_reason = "error";
        return result;
    }

    return generate_after_prefill(*sampler, params, std::move(on_token), cancel);
}

/**
 * @brief Post-prefill sampling loop (gh#98, v2.8.0 extraction from decode_loop).
 *
 * The prompt is already prefilled into seq 0; runs step_token until EOS / stop
 * / max_tokens / cancel. Shared by decode_loop (after run_prefill) and the
 * same-prefix batch path (after a suffix decode_tokens_from).
 *
 * @param sampler Per-request sampler (carries its grammar).
 * @param params Generation parameters (max_tokens, stop).
 * @param on_token Streaming callback (empty for batch).
 * @param cancel Cancel flag (nullptr for batch).
 * @return GenerationResult with content/finish_reason/token_count.
 * @internal
 * @version 2.8.3
 */
GenerationResult LlamaCppBackend::generate_after_prefill(
    Sampler& sampler,
    const GenerationParams& params,
    std::function<void(std::string_view)> on_token,
    std::atomic<bool>* cancel)
{
    GenerationResult result;
    std::string generated;
    int n_generated = 0;
    const auto stop = effective_stop(params);  // gh#105: per-call sequential marker

    while (n_generated < params.max_tokens) {
        bool cancelled = cancel && cancel->load(std::memory_order_acquire);
        if (cancelled) {
            result.finish_reason = "cancelled";
            result.error_code = ENTROPIC_ERROR_CANCELLED;
            break;
        }

        auto status = step_token(sampler, generated, on_token, stop);
        if (status == "continue") {
            ++n_generated;
        } else {
            result.finish_reason = (status == "error") ? "error" : "stop";
            if (status == "error") {
                result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
            }
            break;
        }
    }

    if (n_generated >= params.max_tokens && result.finish_reason.empty()) {
        result.finish_reason = "length";
    }

    result.content = generated;
    result.token_count = n_generated;
    return result;
}

// ── gh#98: same-prefix multi-seq batched generation ────────

/**
 * @brief Build a single error GenerationResult (gh#98 batch failures).
 * @utility
 * @version 2.8.0
 */
static GenerationResult batch_error_result(const std::string& msg) {
    GenerationResult e;
    e.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
    e.error_message = msg;
    e.finish_reason = "error";
    return e;
}

/**
 * @brief Fill one cell of a multi-seq llama_batch.
 * @utility
 * @version 2.8.0
 */
static void fill_batch_cell(llama_batch& b, int k, llama_token tok,
                            llama_pos pos, llama_seq_id seq, bool want_logits) {
    b.token[k] = tok;
    b.pos[k] = pos;
    b.n_seq_id[k] = 1;
    b.seq_id[k][0] = seq;
    b.logits[k] = want_logits ? 1 : 0;
}

/**
 * @brief Build per-request sampler chains + KV sequence ids (gh#98).
 * @return false if any sampler chain could not be built.
 * @internal
 * @version 2.8.0
 */
bool LlamaCppBackend::prepare_batch_seqs(
    std::vector<BatchSeq>& seqs,
    const std::vector<GenerationParams>& params) {
    for (std::size_t i = 0; i < seqs.size(); ++i) {
        seqs[i].sampler = create_sampler(params[i]);
        auto* ls = dynamic_cast<LlamaCppSampler*>(seqs[i].sampler.get());
        if (ls == nullptr) { return false; }
        seqs[i].chain = ls->native_chain();
        seqs[i].seq_id = (i == 0) ? 0 : allocate_temp_seq_id();
        seqs[i].max_tokens = params[i].max_tokens;
    }
    return true;
}

/**
 * @brief Prefill the shared prefix into seq 0 and seq_cp it to the others.
 * @internal
 * @version 2.8.0
 */
bool LlamaCppBackend::prefill_shared_and_fanout(
    std::vector<BatchSeq>& seqs, const std::vector<llama_token>& seq0,
    std::size_t shared) {
    std::vector<llama_token> prefix(
        seq0.begin(), seq0.begin() + static_cast<long>(shared));
    if (!decode_tokens_from(prefix, 0)) { return false; }  // into seq 0
    auto* mem = llama_get_memory(ctx_);
    for (std::size_t i = 1; i < seqs.size(); ++i) {
        llama_memory_seq_cp(mem, 0, seqs[i].seq_id, 0,
                            static_cast<llama_pos>(shared));
    }
    for (auto& s : seqs) { s.pos = static_cast<int>(shared); }
    return true;
}

/**
 * @brief Prefill every request's suffix in one multi-seq batch (gh#98).
 *
 * Each sequence's suffix tokens are decoded at their real positions; only the
 * last token of each carries logits, recorded as that seq's first sample idx.
 * @internal
 * @version 2.8.0
 */
bool LlamaCppBackend::prefill_batch_suffixes(
    std::vector<BatchSeq>& seqs,
    const std::vector<std::vector<llama_token>>& toks,
    std::size_t shared) {
    int total = 0;
    // shared <= shortest-1 < every t.size() by batch_shared_prefix_len, but
    // guard the unsigned subtraction defensively (a bad `shared` would else
    // underflow to a huge alloc).
    for (const auto& t : toks) {
        total += static_cast<int>(t.size() - std::min(shared, t.size()));
    }
    llama_batch batch = llama_batch_init(total, 0,
                                         static_cast<int32_t>(seqs.size()));
    int k = 0;
    for (std::size_t i = 0; i < seqs.size(); ++i) {
        int len = static_cast<int>(toks[i].size());
        for (int p = static_cast<int>(shared); p < len; ++p) {
            fill_batch_cell(batch, k, toks[i][p], p, seqs[i].seq_id,
                            p == len - 1);
            if (p == len - 1) { seqs[i].logits_idx = k; }
            ++k;
        }
        seqs[i].pos = len;
    }
    batch.n_tokens = k;
    last_prefill_tokens_ += k;
    bool ok = (llama_decode(ctx_, batch) == 0);
    llama_batch_free(batch);
    return ok;
}

/**
 * @brief Sample + accept + classify each still-active sequence (gh#98).
 * @internal
 * @version 2.8.0
 */
void LlamaCppBackend::sample_batch_active(std::vector<BatchSeq>& seqs) {
    for (auto& s : seqs) {
        if (!s.active) { continue; }
        // llama_sampler_sample() accepts the drawn token into the chain
        // internally (advancing grammar/penalties) — matching the single-seq
        // step_token path. A second accept would double-advance the grammar.
        llama_token tok = llama_sampler_sample(s.chain, ctx_, s.logits_idx);
        if (llama_vocab_is_eog(vocab_, tok)) {
            s.active = false;
            s.finish = "stop";
            continue;
        }
        s.out.push_back(tok);
        ++s.n_gen;
        if (s.n_gen >= s.max_tokens) { s.active = false; s.finish = "length"; }
    }
}

/**
 * @brief Decode all sequences together until each finishes (gh#98).
 *
 * One `llama_decode` per step over the still-active sequences' just-sampled
 * tokens — the multi-seq throughput win. `last_gen_decode_calls_` counts the
 * decodes (≈ longest output), the observable that batching engaged (vs N·len
 * for a serial fallback).
 * @internal
 * @version 2.8.0
 */
void LlamaCppBackend::run_batch_gen_loop(
    std::vector<BatchSeq>& seqs, int max_steps, std::atomic<bool>& cancel) {
    llama_batch batch = llama_batch_init(static_cast<int32_t>(seqs.size()), 0,
                                         static_cast<int32_t>(seqs.size()));
    for (int step = 0; step < max_steps; ++step) {
        if (cancel.load(std::memory_order_acquire)) { break; }
        sample_batch_active(seqs);
        int k = 0;
        for (auto& s : seqs) {
            if (!s.active) { continue; }
            fill_batch_cell(batch, k, s.out.back(), s.pos, s.seq_id, true);
            s.logits_idx = k;
            ++s.pos;
            ++k;
        }
        if (k == 0) { break; }
        batch.n_tokens = k;
        ++last_gen_decode_calls_;
        if (llama_decode(ctx_, batch) != 0) { break; }
    }
    llama_batch_free(batch);
}

/**
 * @brief Detokenize each sequence into a GenerationResult (gh#98).
 * @internal
 * @version 2.8.0
 */
std::vector<GenerationResult> LlamaCppBackend::build_batch_results(
    std::vector<BatchSeq>& seqs) {
    std::vector<GenerationResult> out;
    out.reserve(seqs.size());
    for (auto& s : seqs) {
        GenerationResult r;
        for (llama_token t : s.out) { r.content += detokenize(t); }
        r.token_count = s.n_gen;
        r.finish_reason = s.finish;
        out.push_back(std::move(r));
    }
    return out;
}

/**
 * @brief Release every batch sequence's temp seq_id (seq 0 excluded, gh#98).
 * @internal
 * @version 2.8.0
 */
void LlamaCppBackend::release_temp_seqs(std::vector<BatchSeq>& seqs) {
    for (std::size_t i = 1; i < seqs.size(); ++i) {
        if (seqs[i].seq_id != 0) { release_temp_seq_id(seqs[i].seq_id); }
    }
}

/**
 * @brief Run the gh#98 multi-seq batched decode (v2.8.0).
 *
 * Prefill shared once → seq_cp fan-out → batched suffix prefill → batched
 * generation loop (one decode/step over N sequences, each sampled under its
 * own grammar). `last_prefill_tokens_` holds `shared + Σ suffix` (prefix
 * prefilled once); `last_gen_decode_calls_` holds the batched step count.
 *
 * @internal
 * @version 2.8.0
 */
std::vector<GenerationResult> LlamaCppBackend::run_batched_decode(
    const std::vector<std::vector<llama_token>>& toks,
    const std::vector<GenerationParams>& params,
    std::size_t shared,
    std::atomic<bool>& cancel)
{
    const std::size_t n = toks.size();
    std::vector<BatchSeq> seqs(n);
    if (!prepare_batch_seqs(seqs, params)) {
        release_temp_seqs(seqs);  // don't leak ids allocated before the failure
        return std::vector<GenerationResult>(
            n, batch_error_result("batch sampler init"));
    }
    int max_steps = 0;
    for (const auto& p : params) { max_steps = std::max(max_steps, p.max_tokens); }

    llama_memory_clear(llama_get_memory(ctx_), true);
    invalidate_resident_kv();
    last_prefill_tokens_ = 0;
    last_gen_decode_calls_ = 0;

    bool ok = prefill_shared_and_fanout(seqs, toks[0], shared)
           && prefill_batch_suffixes(seqs, toks, shared);
    if (ok) { run_batch_gen_loop(seqs, max_steps, cancel); }

    auto out = ok ? build_batch_results(seqs)
                  : std::vector<GenerationResult>(
                        n, batch_error_result("batch prefill"));
    release_temp_seqs(seqs);
    invalidate_resident_kv();
    logger->info("gh#98 batch: requests={} prefix.tokens_shared={} "
                 "prefix.tokens_saved={} total_prefill_tokens={} gen_decodes={}",
                 n, shared, shared * (n - 1), last_prefill_tokens_,
                 last_gen_decode_calls_);
    return out;
}

/**
 * @brief Same-prefix batch generation override (gh#98, v2.8.0).
 *
 * Tokenizes each request, computes the shared prefix, and takes the batch
 * fast-path when batch_is_viable (plain KV, real shared prefix, fits the seq
 * slots + decode batch); otherwise falls back to the serial base path. The
 * serial fallback is always correct — it is the unmodified single-request path
 * per request — so disjoint prompts, hybrid archs, and over-capacity batches
 * degrade gracefully with no regression.
 *
 * @param requests Per-request message lists.
 * @param params Per-request generation params.
 * @param cancel Cancel flag.
 * @return One result per request, in input order.
 * @internal
 * @version 2.8.0
 */
std::vector<GenerationResult> LlamaCppBackend::do_generate_batch(
    const std::vector<std::vector<Message>>& requests,
    const std::vector<GenerationParams>& params,
    std::atomic<bool>& cancel)
{
    const std::size_t n = requests.size();
    std::vector<std::vector<llama_token>> toks(n);
    for (std::size_t i = 0; i < n; ++i) {
        toks[i] = tokenize(render_prompt(requests[i], params[i]), true);
    }
    const std::size_t shared = batch_shared_prefix_len(toks);
    std::size_t total_suffix = 0;
    for (const auto& t : toks) { total_suffix += t.size() - shared; }

    const bool hybrid = is_hybrid_ || is_recurrent_;
    if (!batch_is_viable(n, config().n_parallel, shared, hybrid,
                         total_suffix, config().n_batch)) {
        return InferenceBackend::do_generate_batch(requests, params, cancel);
    }
    return run_batched_decode(toks, params, shared, cancel);
}

// ── Prompt cache helpers ───────────────────────────────────

/**
 * @brief Extract system prompt text from message list.
 * @param messages Conversation history.
 * @return System message content, empty if none found.
 * @internal
 * @version 1.8.3
 */
std::string LlamaCppBackend::extract_system_prompt(
    const std::vector<Message>& messages)
{
    for (const auto& msg : messages) {
        if (msg.role == "system") {
            return msg.content;
        }
    }
    return "";
}

/**
 * @brief Decode remaining tokens starting at `start_offset`.
 *
 * Assumes `seq_pos_max(0) == start_offset - 1` so that
 * `llama_batch_get_one` auto-positions tokens at start_offset onward.
 *
 * @param tokens Full token sequence.
 * @param start_offset Index of the first token to decode.
 * @return true on success, false on decode failure.
 * @internal
 * @version 2.7.5
 */
bool LlamaCppBackend::decode_tokens_from(
    const std::vector<llama_token>& tokens, int start_offset)
{
    int total = static_cast<int>(tokens.size());
    if (start_offset >= total) { return true; }

    int n_batch = llama_n_batch(ctx_);
    int n_remaining = total - start_offset;
    last_prefill_tokens_ += n_remaining;  // gh#96: count tokens decoded here
    for (int off = 0; off < n_remaining; off += n_batch) {
        int chunk = std::min(n_batch, n_remaining - off);
        llama_batch batch = llama_batch_get_one(
            const_cast<llama_token*>(tokens.data())
                + start_offset + off,
            chunk);
        if (llama_decode(ctx_, batch) != 0) {
            logger->error("Decode chunk failed (start={}, off={}, "
                          "chunk={})", start_offset, off, chunk);
            return false;
        }
    }
    return true;
}

/**
 * @brief Restore cached prefix KV and decode remaining tokens.
 *
 * After v2.0.6 the cached state contains ONLY the system prefix
 * (two-pass prefill saves at the prefix boundary, see
 * `prefill_and_cache_prefix`). Restore is therefore clean by
 * construction: `llama_state_seq_set_data` leaves seq 0 with exactly
 * `cached->token_count` positions filled, and `llama_batch_get_one`
 * auto-positions subsequent decodes at that boundary.
 *
 * @param cached Cache entry to restore from.
 * @param tokens Full token sequence.
 * @return true on success, false to fall back to full prefill.
 * @internal
 * @version 2.0.6
 */
bool LlamaCppBackend::restore_cached_prefix(
    const CacheEntry* cached,
    const std::vector<llama_token>& tokens)
{
    auto* mem = llama_get_memory(ctx_);
    llama_memory_clear(mem, true);

    size_t restored = llama_state_seq_set_data(
        ctx_, cached->data.data(), cached->data_size, 0);
    if (restored == 0) {
        logger->warn("KV state restore failed, falling back to full prefill");
        return false;
    }

    return decode_tokens_from(tokens, cached->token_count);
}

/**
 * @brief Capture seq 0 KV state and store under the given key.
 *
 * The caller is responsible for ensuring seq 0 currently contains
 * exactly `prefix_tokens` positions — this function trusts that
 * contract and serializes whatever is there.
 *
 * @param key Cache key for the prefix.
 * @param prefix_tokens Number of prefix tokens currently in seq 0.
 * @internal
 * @version 2.0.6
 */
void LlamaCppBackend::save_prefix_to_cache(
    const CacheKey& key, int prefix_tokens)
{
    size_t state_size = llama_state_seq_get_size(ctx_, 0);
    if (state_size == 0) {
        return;
    }

    std::vector<uint8_t> buf(state_size);
    size_t written = llama_state_seq_get_data(
        ctx_, buf.data(), buf.size(), 0);
    if (written > 0) {
        buf.resize(written);
        prompt_cache_->store(key, std::move(buf), prefix_tokens);
    }
}

/**
 * @brief Compute system prefix token count from messages.
 * @param messages Original message list.
 * @param params Generation params (for template).
 * @return Token count of the system prefix, 0 if no system message.
 * @internal
 * @version 1.8.3
 */
int LlamaCppBackend::compute_prefix_token_count(
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    std::vector<Message> sys_msgs;
    for (const auto& msg : messages) {
        if (msg.role == "system") {
            sys_msgs.push_back(msg);
        }
    }
    if (sys_msgs.empty()) {
        return 0;
    }

    std::string sys_prompt = apply_chat_template(sys_msgs, params);
    auto sys_tokens = tokenize(sys_prompt, true);
    return static_cast<int>(sys_tokens.size());
}

/**
 * @brief Prefill in two passes: prefix → save → remainder.
 *
 * The v2.0.6 correctness fix. `llama_state_seq_get_data` has no
 * range parameter, so any save captures whatever KV state happens
 * to be in seq 0 at save time. By prefilling ONLY the system prefix
 * first, saving, and then continuing with the rest of the prompt, we
 * guarantee the cache entry covers exactly `prefix_tokens` positions
 * — no residue from later conversation tokens can leak into a
 * subsequent delegation's cache hit.
 *
 * If `prefix_tokens` is 0 or >= total tokens, falls back to a plain
 * full prefill without caching (nothing meaningful to cache).
 *
 * @param tokens Full token sequence.
 * @param prefix_tokens System prefix token count.
 * @param key Cache key for the prefix.
 * @return true on success.
 * @internal
 * @version 2.0.6
 */
bool LlamaCppBackend::prefill_and_cache_prefix(
    const std::vector<llama_token>& tokens,
    int prefix_tokens,
    const CacheKey& key)
{
    int total = static_cast<int>(tokens.size());
    if (prefix_tokens <= 0 || prefix_tokens >= total) {
        return run_prefill(tokens);
    }

    // Pass 1: prefill only the prefix — `run_prefill` calls
    // llama_memory_clear, so seq 0 ends up holding exactly
    // prefix_tokens positions.
    std::vector<llama_token> prefix(
        tokens.begin(), tokens.begin() + prefix_tokens);
    if (!run_prefill(prefix)) {
        return false;
    }

    // Save now: state contains exactly the prefix.
    save_prefix_to_cache(key, prefix_tokens);

    // Pass 2: continue prefilling the remainder. No clear — decode
    // appends after the saved prefix positions.
    return decode_tokens_from(tokens, prefix_tokens);
}

/**
 * @brief Run prefill with prompt cache integration (perf-instrumented wrapper).
 *
 * Resets the llama perf counters, dispatches to the cache-aware prefill
 * (prefill_dispatch), then captures this generation's prompt-eval token
 * count (gh#96). Thin wrapper so the dispatch body stays under the knots
 * SLOC gate.
 *
 * @param tokens Full token sequence.
 * @param system_prompt System prompt text for cache key.
 * @param messages Original messages (for prefix boundary).
 * @param params Generation parameters.
 * @return true on success.
 * @internal
 * @version 2.7.6
 */
bool LlamaCppBackend::run_prefill_cached(
    const std::vector<llama_token>& tokens,
    const std::string& system_prompt,
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    // gh#96 (v2.7.5): count tokens actually pushed through llama_decode during
    // prefill this turn. run_prefill / decode_tokens_from accumulate into
    // last_prefill_tokens_; a prompt-cache HIT restores the system prefix
    // without a decode, so this counts the re-decoded post-system remainder —
    // the per-turn waste that climbs today and should collapse to the appended
    // delta once warm-keep reuse lands. (llama_perf n_p_eval proved unreliable
    // across the state-restore boundary, so we count the decodes directly.)
    last_prefill_tokens_ = 0;
    last_input_tokens_ = static_cast<int>(tokens.size());  // gh#97
    auto t_pre = entropic::log::now();
    bool ok;
    if (is_hybrid_ || is_recurrent_) {
        // gh#97 (v2.7.6): hybrid/recurrent (SSM) memory rejects the partial
        // seq_rm warm-keep needs (state can't be partially erased at the tail),
        // and the prompt-cache restore lands non-contiguous cells — both desync
        // KV positions (pos_max inflates → eventual decode slot-failure with the
        // cache mostly empty). Plain full prefill (clear + contiguous decode) is
        // the only correct path for these archs at this llama.cpp pin. Mirrors
        // the speculative-decoding guard. Forfeits the gh#96 reuse for them.
        ok = run_prefill(tokens);
        invalidate_resident_kv();
    } else {
        // gh#96 warm-keep: reuse the resident KV prefix + decode only the delta;
        // fall back to a cold prefill (clear + system-prefix cache) when reuse
        // is off, the prefix diverged, or the KV was mutated out-of-band.
        ok = try_warm_reuse(tokens);
        if (!ok) {
            ok = prefill_dispatch(tokens, system_prompt, messages, params);
            if (ok) {
                resident_tokens_ = tokens;
            } else {
                invalidate_resident_kv();
            }
        }
    }
    last_prefill_ms_ = entropic::log::elapsed_ms(t_pre, entropic::log::now());
    logger->info("Prefill (gh#96): {} tokens / {:.1f} ms decoded this turn",
                 last_prefill_tokens_, last_prefill_ms_);
    return ok;
}

/**
 * @brief gh#96 (v2.7.5): incremental prefill against resident KV.
 *
 * See header. The seq_rm here operates on LIVE (never state-restored) KV
 * cells, so the partial-removal restriction noted in architecture decision
 * #35 (non-contiguous restored cells) does not apply. decode_tokens_from
 * auto-positions the delta at `cut` (same mechanism the cache-restore path
 * relies on). Occupancy is derived from llama_memory_seq_pos_max, never a
 * software counter — so an out-of-band wipe (multimodal / complete /
 * speculative / a different conversation interleaved on a shared backend)
 * either fails the warm_keep_cut occupancy gate or diverges in the prefix
 * scan, and we fall back. Per-turn cost shrinks from the whole post-system
 * history to just the appended delta.
 *
 * @param tokens Full incoming token sequence.
 * @return true if reuse handled the prefill; false to fall back (no KV change).
 * @internal
 * @version 2.7.5
 */
bool LlamaCppBackend::try_warm_reuse(const std::vector<llama_token>& tokens) {
    if (!prompt_cache_config_.warm_keep || ctx_ == nullptr) {
        return false;
    }
    auto* mem = llama_get_memory(ctx_);
    long pos_max = static_cast<long>(llama_memory_seq_pos_max(mem, 0));
    std::size_t cut = warm_keep_cut(resident_tokens_, tokens, pos_max);
    if (cut == 0) {
        return false;  // nothing reusable — cold prefill
    }
    // Drop the divergent tail (and any prior generated tokens past `cut`),
    // then decode only the appended delta. A single exit (returns <= 3 gate):
    // success records the new resident set; failure invalidates and reports it.
    llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(cut), -1);
    bool ok = decode_tokens_from(tokens, static_cast<int>(cut));
    if (ok) {
        resident_tokens_ = tokens;
        if (prompt_cache_config_.log_hits) {
            logger->info("Warm-keep: reused {} resident tokens, decoded {} "
                         "delta (of {} total)", cut, tokens.size() - cut,
                         tokens.size());
        }
    } else {
        invalidate_resident_kv();
    }
    return ok;
}

/**
 * @brief gh#96 (v2.7.5): drop the warm-keep resident-KV record.
 *
 * Out-of-line so the header carries no inline body here (an inline body in
 * this region makes the knots complexity counter attribute the whole
 * declaration run to the preceding declaration).
 * @utility
 * @internal
 * @version 2.7.5
 */
void LlamaCppBackend::invalidate_resident_kv() {
    resident_tokens_.clear();
}

/**
 * @brief Cache-aware prefill dispatch (pre-v2.7.5 run_prefill_cached body).
 *
 * On cache hit: restore prefix KV and decode the remainder.
 * On cache miss: two-pass prefill (prefix → save → remainder) so the stored
 * cache entry contains prefix-only state. Extracted from run_prefill_cached
 * in v2.7.5 so the wrapper can own the gh#96 perf reset + capture.
 *
 * @param tokens Full token sequence.
 * @param system_prompt System prompt text for cache key.
 * @param messages Original messages (for prefix boundary).
 * @param params Generation parameters.
 * @return true on success.
 * @internal
 * @version 2.7.5
 */
bool LlamaCppBackend::prefill_dispatch(
    const std::vector<llama_token>& tokens,
    const std::string& system_prompt,
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    bool cache_enabled = prompt_cache_
        && prompt_cache_config_.enabled
        && !system_prompt.empty();

    if (!cache_enabled) {
        return run_prefill(tokens);
    }

    CacheKey key = PromptCache::make_key(
        system_prompt, config().path.string());
    const CacheEntry* cached = prompt_cache_->lookup(key);

    if (cached != nullptr) {
        if (prompt_cache_config_.log_hits) {
            logger->info("Prompt cache HIT: {} bytes, {} prefix tokens",
                         cached->data_size, cached->token_count);
        }
        if (restore_cached_prefix(cached, tokens)) {
            return true;
        }
        logger->warn("Cache restore failed, falling back to full prefill");
    } else if (prompt_cache_config_.log_hits) {
        logger->info("Prompt cache MISS: processing full prompt");
    }

    int prefix_tokens = compute_prefix_token_count(messages, params);
    return prefill_and_cache_prefix(tokens, prefix_tokens, key);
}

// ── Multimodal generation (v1.9.11 Phases 5–7 + v2.1.8) ────

namespace {

/**
 * @brief True when any message carries an IMAGE content part.
 * @internal
 * @version 2.1.8
 */
bool any_image_in(const std::vector<Message>& messages) {
    for (const auto& m : messages) {
        if (has_images(m.content_parts)) { return true; }
    }
    return false;
}

/**
 * @brief Strip image content_parts down to text-only (fallback path).
 *
 * Used when a non-vision model receives image content. Each message's
 * `content_parts` is replaced with `extract_text(parts)` so the
 * model sees the surrounding prose but no image references.
 *
 * @param messages Original messages.
 * @return New message vector with image parts removed.
 * @internal
 * @version 2.1.8
 */
std::vector<Message> strip_image_parts(
    const std::vector<Message>& messages) {
    std::vector<Message> out = messages;
    for (auto& m : out) {
        if (m.content_parts.empty()) { continue; }
        m.content = extract_text(m.content_parts);
        m.content_parts.clear();
    }
    return out;
}

/**
 * @brief Substitute IMAGE parts with the mtmd media marker.
 *
 * The marker (typically `<__media__>`) tells mtmd_tokenize where
 * to splice in the encoded image embeddings. Image bitmaps are
 * loaded via mtmd_helper_bitmap_init_from_file and accumulated into
 * `bitmaps_out` in part order.
 *
 * @param messages Original messages (with content_parts).
 * @param ctx Active mtmd context (used to load bitmaps).
 * @param bitmaps_out Accumulator for loaded bitmap pointers (caller
 *        owns; must mtmd_bitmap_free each on exit).
 * @return Messages with content flattened to marker-substituted text,
 *         or empty vector if any image fails to load.
 * @internal
 * @version 2.1.8
 */
std::vector<Message> substitute_image_markers(
    const std::vector<Message>& messages,
    ::mtmd_context* ctx,
    std::vector<::mtmd_bitmap*>& bitmaps_out) {
    std::vector<Message> out;
    out.reserve(messages.size());
    const std::string marker = mtmd_default_marker();
    for (const auto& m : messages) {
        Message copy;
        copy.role = m.role;
        if (m.content_parts.empty()) {
            copy.content = m.content;
            out.push_back(std::move(copy));
            continue;
        }
        std::string built;
        for (const auto& p : m.content_parts) {
            if (p.type != ContentPartType::IMAGE) {
                built += p.text;
                continue;
            }
            ::mtmd_bitmap* bm = nullptr;
            if (!p.image_path.empty()) {
                bm = mtmd_helper_bitmap_init_from_file(
                    ctx, p.image_path.c_str());
            }
            if (bm == nullptr) { return {}; }
            bitmaps_out.push_back(bm);
            built += marker;
        }
        copy.content = std::move(built);
        out.push_back(std::move(copy));
    }
    return out;
}

} // anonymous namespace

/**
 * @brief mtmd prefill helper (v2.1.8) — tokenize + eval chunks.
 *
 * Wraps mtmd_tokenize + mtmd_helper_eval_chunks. KV cache is cleared
 * first so prefill always starts at seq position 0. Bitmap ownership
 * stays with the caller (mtmd_tokenize borrows for the call).
 *
 * @internal
 * @version 2.1.8
 */
entropic_error_t LlamaCppBackend::mtmd_prefill(
    const std::string& prompt,
    const std::vector<::mtmd_bitmap*>& bitmaps,
    std::string& err_msg)
{
    llama_memory_clear(llama_get_memory(ctx_), true);
    ::mtmd_input_text mt{prompt.c_str(), true, true};
    auto* chunks = mtmd_input_chunks_init();
    std::vector<const ::mtmd_bitmap*> bm_cptrs(
        bitmaps.begin(), bitmaps.end());
    int32_t tok_rc = mtmd_tokenize(
        mtmd_ctx_, chunks, &mt, bm_cptrs.data(), bm_cptrs.size());
    if (tok_rc != 0) {
        mtmd_input_chunks_free(chunks);
        err_msg = "mtmd_tokenize failed (rc="
            + std::to_string(tok_rc) + ")";
        return ENTROPIC_ERROR_GENERATE_FAILED;
    }
    llama_pos new_n_past = 0;
    int32_t eval_rc = mtmd_helper_eval_chunks(
        mtmd_ctx_, ctx_, chunks, 0, 0,
        static_cast<int32_t>(config().n_batch),
        true, &new_n_past);
    mtmd_input_chunks_free(chunks);
    if (eval_rc != 0) {
        err_msg = "mtmd_helper_eval_chunks failed (rc="
            + std::to_string(eval_rc) + ")";
        return ENTROPIC_ERROR_GENERATE_FAILED;
    }
    logger->info("Multimodal prefill complete: n_past={}", new_n_past);
    return ENTROPIC_OK;
}

/**
 * @brief Shared sampling loop (v2.1.8).
 *
 * Operates on the already-positioned `ctx_` KV cache. Mirrors the
 * body of both text-only generation variants but factored out so
 * generate_multimodal can reuse it after mtmd_prefill.
 *
 * @internal
 * @version 2.8.3
 */
GenerationResult LlamaCppBackend::run_sampling_loop(
    const GenerationParams& params,
    std::function<void(std::string_view token)> on_token,
    std::atomic<bool>* cancel,
    const std::chrono::steady_clock::time_point& t0)
{
    GenerationResult result;
    // v2.3.10: Sampler seam.
    auto sampler = create_sampler(params);
    if (!sampler) {
        result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        result.error_message = "Sampler factory not initialized";
        result.finish_reason = "error";
        finalize_result(result, t0);
        return result;
    }
    std::string generated;
    int n_generated = 0;
    const auto stop = effective_stop(params);  // gh#105: per-call sequential marker
    while (n_generated < params.max_tokens) {
        if (cancel != nullptr
                && cancel->load(std::memory_order_acquire)) {
            result.finish_reason = "cancelled";
            result.error_code = ENTROPIC_ERROR_CANCELLED;
            break;
        }
        auto status = step_token(
            *sampler, generated, on_token, stop);
        if (status == "continue") { ++n_generated; continue; }
        result.finish_reason = (status == "error") ? "error" : "stop";
        if (status == "error") {
            result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        }
        break;
    }
    finalize_generation(result, generated, n_generated, params, t0);
    return result;
}

/**
 * @brief Multimodal generation core (v2.1.8, gh#37 / v1.9.11 Phase 6).
 * @internal
 * @version 2.7.5
 */
GenerationResult LlamaCppBackend::generate_multimodal(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view token)> on_token,
    std::atomic<bool>* cancel)
{
    auto t0 = entropic::log::now();
    invalidate_resident_kv();  // gh#96: mtmd_prefill mutates seq 0 out-of-band
    std::vector<::mtmd_bitmap*> bitmaps;
    auto marked = substitute_image_markers(
        messages, mtmd_ctx_, bitmaps);
    if (marked.empty()) {
        for (auto* b : bitmaps) { mtmd_bitmap_free(b); }
        GenerationResult err;
        err.error_code = ENTROPIC_ERROR_IMAGE_LOAD_FAILED;
        err.error_message =
            "mtmd_helper_bitmap_init_from_file failed";
        return err;
    }
    auto prompt = render_prompt(marked, params);
    logger->info("Multimodal generate: {} images, prompt={} chars, max_tokens={}",
                 bitmaps.size(), prompt.size(), params.max_tokens);
    std::string prefill_err;
    auto rc = mtmd_prefill(prompt, bitmaps, prefill_err);
    for (auto* b : bitmaps) { mtmd_bitmap_free(b); }
    if (rc != ENTROPIC_OK) {
        GenerationResult err;
        err.error_code = rc;
        err.error_message = std::move(prefill_err);
        return err;
    }
    return run_sampling_loop(params, on_token, cancel, t0);
}

// ── Generation entry points ────────────────────────────────

/**
 * @brief Generate a complete response using chat template.
 *
 * v2.1.8 (gh#37 / v1.9.11 Phases 5–7): dispatches to
 * generate_multimodal() when any message carries IMAGE
 * content_parts AND the backend has vision (mmproj loaded). When
 * images arrive but vision is not available, image parts are
 * stripped with a warning and generation proceeds text-only.
 *
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @return GenerationResult.
 * @internal
 * @version 2.1.8
 */
GenerationResult LlamaCppBackend::do_generate(
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    if (!any_image_in(messages)) {
        return do_generate_text_only(messages, params);
    }
    if (has_vision_ && mtmd_ctx_ != nullptr) {
        return generate_multimodal(messages, params, nullptr, nullptr);
    }
    logger->warn("Image content present but model has no vision "
                 "capability — stripping image parts");
    return do_generate_text_only(strip_image_parts(messages), params);
}

/**
 * @brief Text-only generate body (v2.1.8, extracted for knots SLOC).
 * @internal
 * @version 2.8.3
 */
GenerationResult LlamaCppBackend::do_generate_text_only(
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    auto t0 = entropic::log::now();
    std::string prompt = render_prompt(messages, params);
    auto tokens = tokenize(prompt, true);
    std::string sys = extract_system_prompt(messages);

    logger->info("Generate: {} input tokens, max_tokens={}",
              tokens.size(), params.max_tokens);
    log_sampler_config(params);

    // v2.3.10: Sampler seam.
    auto sampler = create_sampler(params);
    if (!sampler) { return sampler_init_error(t0); }

    if (!run_prefill_cached(tokens, sys, messages, params)) {
        return prefill_error();
    }

    GenerationResult result;
    std::string generated;
    int n_generated = 0;
    std::function<void(std::string_view)> no_cb = nullptr;
    const auto stop = effective_stop(params);  // gh#105: per-call sequential marker

    while (n_generated < params.max_tokens) {
        auto status = step_token(
            *sampler, generated, no_cb, stop);
        if (status == "continue") { ++n_generated; }
        else {
            result.finish_reason =
                (status == "error") ? "error" : "stop";
            if (status == "error") {
                result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
            }
            break;
        }
    }

    finalize_generation(result, generated, n_generated, params, t0);
    return result;
}

/**
 * @brief Batch generate dispatch with cancel support (gh#81, v2.4.2).
 *
 * Vision branch falls through to the streaming multimodal kernel with
 * a null on_token (it already polls cancel). Text-only branch goes
 * through `do_generate_text_only(messages, params, cancel)`.
 *
 * @internal
 * @version 2.4.2
 */
GenerationResult LlamaCppBackend::do_generate(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::atomic<bool>& cancel)
{
    if (!any_image_in(messages)) {
        return do_generate_text_only(messages, params, cancel);
    }
    if (has_vision_ && mtmd_ctx_ != nullptr) {
        return generate_multimodal(messages, params, nullptr, &cancel);
    }
    logger->warn("Image content present but model has no vision "
                 "capability — stripping image parts");
    return do_generate_text_only(strip_image_parts(messages), params, cancel);
}

/**
 * @brief Text-only batch generate with per-token cancel poll (gh#81, v2.4.2).
 *
 * Identical to the no-cancel overload above except for the cancel
 * poll inside the decode loop. See its docs for the prefill / sampler
 * contract.
 *
 * @internal
 * @version 2.8.3
 */
GenerationResult LlamaCppBackend::do_generate_text_only(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::atomic<bool>& cancel)
{
    auto t0 = entropic::log::now();
    std::string prompt = render_prompt(messages, params);
    auto tokens = tokenize(prompt, true);
    std::string sys = extract_system_prompt(messages);

    logger->info("Generate (cancellable): {} input tokens, max_tokens={}",
              tokens.size(), params.max_tokens);
    log_sampler_config(params);

    auto sampler = create_sampler(params);
    if (!sampler) { return sampler_init_error(t0); }

    if (!run_prefill_cached(tokens, sys, messages, params)) {
        return prefill_error();
    }

    GenerationResult result;
    std::string generated;
    int n_generated = 0;
    std::function<void(std::string_view)> no_cb = nullptr;

    const auto stop = effective_stop(params);  // gh#105: per-call sequential marker
    while (n_generated < params.max_tokens) {
        if (cancel.load(std::memory_order_acquire)) {
            result.finish_reason = "cancelled";
            result.error_code = ENTROPIC_ERROR_CANCELLED;
            break;
        }
        auto status = step_token(
            *sampler, generated, no_cb, stop);
        if (status == "continue") { ++n_generated; }
        else {
            result.finish_reason =
                (status == "error") ? "error" : "stop";
            if (status == "error") {
                result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
            }
            break;
        }
    }

    finalize_generation(result, generated, n_generated, params, t0);
    return result;
}

/**
 * @brief Streaming generation with per-token callback.
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param on_token Per-token callback.
 * @param cancel Atomic cancel flag.
 * @return GenerationResult.
 * @internal
 * @version 2.1.8
 */
GenerationResult LlamaCppBackend::do_generate_streaming(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view token)> on_token,
    std::atomic<bool>& cancel)
{
    if (!any_image_in(messages)) {
        return do_generate_streaming_text_only(
            messages, params, on_token, cancel);
    }
    if (has_vision_ && mtmd_ctx_ != nullptr) {
        return generate_multimodal(messages, params, on_token, &cancel);
    }
    logger->warn("Image content present but model has no vision "
                 "capability — stripping image parts");
    return do_generate_streaming_text_only(
        strip_image_parts(messages), params, on_token, cancel);
}

/**
 * @brief Text-only streaming body (v2.1.8, extracted for knots SLOC).
 * @internal
 * @version 2.8.3
 */
GenerationResult LlamaCppBackend::do_generate_streaming_text_only(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view token)> on_token,
    std::atomic<bool>& cancel)
{
    auto t0 = entropic::log::now();
    auto prompt = render_prompt(messages, params);
    auto tokens = tokenize(prompt, true);
    auto sys = extract_system_prompt(messages);
    logger->info("Stream: {} input tokens, max_tokens={}",
              tokens.size(), params.max_tokens);
    log_sampler_config(params);

    // v2.3.10: Sampler seam.
    auto sampler = create_sampler(params);
    if (!sampler) { return sampler_init_error(t0); }
    if (!run_prefill_cached(tokens, sys, messages, params)) {
        return prefill_error();
    }
    GenerationResult result;
    std::string generated;
    int n_generated = 0;
    const auto stop = effective_stop(params);  // gh#105: per-call sequential marker
    while (n_generated < params.max_tokens) {
        if (cancel.load(std::memory_order_acquire)) {
            result.finish_reason = "cancelled";
            result.error_code = ENTROPIC_ERROR_CANCELLED;
            break;
        }
        auto status = step_token(
            *sampler, generated, on_token, stop);
        if (status == "continue") { ++n_generated; }
        else {
            result.finish_reason =
                (status == "error") ? "error" : "stop";
            if (status == "error") {
                result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
            }
            break;
        }
    }
    finalize_generation(result, generated, n_generated, params, t0);
    return result;
}

/**
 * @brief Abstract speculative entry point.
 *
 * LlamaCppBackend requires an explicit draft handle, so the abstract
 * single-backend variant returns NOT_SUPPORTED. The orchestrator
 * dynamic_casts both target and draft and calls
 * `generate_speculative_with_draft` directly. (v2.1.11, gh#36)
 *
 * @return GenerationResult with NOT_SUPPORTED.
 * @internal
 * @version 2.1.11 [reviewed]
 */
GenerationResult LlamaCppBackend::do_generate_speculative(
    const std::vector<Message>& /*messages*/,
    const GenerationParams& /*params*/,
    std::function<void(std::string_view)> /*on_token*/,
    std::atomic<bool>& /*cancel*/)
{
    GenerationResult result;
    result.error_code = ENTROPIC_ERROR_NOT_SUPPORTED;
    result.error_message =
        "LlamaCppBackend speculative requires an explicit draft "
        "backend handle — orchestrator dispatches via "
        "generate_speculative_with_draft";
    result.finish_reason = "error";
    return result;
}

namespace {

/**
 * @brief Map entropic GenerationParams → common_params_sampling.
 *
 * The speculative path uses common_sampler (not llama_sampler) so it
 * can call common_sampler_sample_and_accept_n. Sampler config flows
 * through entropic's GenerationParams.
 *
 * @param params Entropic generation params.
 * @return Populated common_params_sampling.
 * @internal
 * @version 2.1.11 [reviewed]
 */
common_params_sampling to_common_sampling(
    const GenerationParams& params) {
    common_params_sampling cps;
    cps.temp = params.temperature;
    cps.top_k = params.top_k;
    cps.top_p = params.top_p;
    cps.penalty_repeat = params.repeat_penalty;
    // gh#23 MVP items 2 + 3 (v2.3.14 + v2.3.15): wire presence +
    // frequency penalty into common-sampling. Counterparts of the
    // 3rd + 4th args to `llama_sampler_init_penalties` in the plain
    // decode path. Default 0.0f on both preserves bit-for-bit
    // speculative output.
    cps.penalty_freq    = params.frequency_penalty;
    cps.penalty_present = params.presence_penalty;
    // gh#23 MVP item 4 (v2.3.16): forward logit_bias to common-sampling.
    // Empty (default) leaves the speculative chain bit-for-bit
    // identical to pre-v2.3.16.
    for (auto& [tok, val] : params.logit_bias) {
        cps.logit_bias.push_back({tok, val});
    }
    if (params.seed >= 0) {
        cps.seed = static_cast<uint32_t>(params.seed);
    }
    cps.no_perf = true;
    // Mirror entropic's standard sampler chain ordering so the
    // speculative path produces output bit-identical to plain decode
    // (the v2.1.11 correctness contract). Entropic's `create_sampler`
    // builds: penalties → top_k → top_p → min_p → temperature → dist,
    // AND SKIPS the temperature sampler when temp == 0 (greedy mode).
    // common_sampler appends an extended-temperature sampler that
    // differs subtly from "no temp at all" — we omit it for temp=0
    // to match entropic exactly. min_p (v2.3.10, gh#23) appended only
    // when caller opted in (params.min_p > 0); 0.0 preserves the
    // pre-v2.3.10 chain shape bit-for-bit. Other extended filters
    // (top_n_sigma, dry, xtc, typical_p) remain stripped.
    cps.samplers = {COMMON_SAMPLER_TYPE_PENALTIES,
                    COMMON_SAMPLER_TYPE_TOP_K,
                    COMMON_SAMPLER_TYPE_TOP_P};
    if (params.min_p > 0.0f) {
        cps.samplers.push_back(COMMON_SAMPLER_TYPE_MIN_P);
    }
    if (params.temperature > 0.0f) {
        cps.samplers.push_back(COMMON_SAMPLER_TYPE_TEMPERATURE);
    }
    cps.min_p = params.min_p;
    cps.dry_multiplier = 0.0f;
    cps.top_n_sigma = -1.0f;
    return cps;
}

/**
 * @brief Prefill a context with all but the last token of `tokens`.
 *
 * Used by the speculative kernel: the target and draft both need
 * `n_past = inp.size() - 1` before the speculation loop, so the loop
 * can build a batch starting with `id_last` (the trailing input
 * token) followed by drafted tokens.
 *
 * Chunks by `llama_n_batch(ctx)` to handle long prompts.
 *
 * @param ctx llama_context to prefill.
 * @param tokens Full input token sequence.
 * @return true on success, false on llama_decode failure.
 * @internal
 * @version 2.1.11
 */
bool spec_prefill_minus_last(
    llama_context* ctx, const std::vector<llama_token>& tokens) {
    int total = static_cast<int>(tokens.size()) - 1;
    if (total <= 0) { return true; }
    int n_batch = llama_n_batch(ctx);
    for (int off = 0; off < total; off += n_batch) {
        int chunk = std::min(n_batch, total - off);
        llama_batch batch = llama_batch_get_one(
            const_cast<llama_token*>(tokens.data()) + off, chunk);
        if (llama_decode(ctx, batch) != 0) { return false; }
    }
    return true;
}

/**
 * @brief Build an error result for kernel-level failures.
 * @internal
 * @version 2.1.11
 */
GenerationResult spec_error(entropic_error_t code, std::string msg) {
    GenerationResult r;
    r.error_code = code;
    r.error_message = std::move(msg);
    r.finish_reason = "error";
    return r;
}

} // anonymous namespace

/**
 * @brief Bundles per-kernel-run mutable state to keep the loop body
 *        focused on its responsibility (knots: cognitive ≤ 15, ≤ 3
 *        returns).
 * @internal
 * @version 2.1.11
 */
struct SpeculativeRunState {
    common_speculative* spec = nullptr;
    common_sampler* smpl = nullptr;
    llama_context* ctx_tgt = nullptr;
    llama_context* ctx_dft = nullptr;
    llama_batch batch_tgt{};
    bool batch_initialized = false;
    llama_seq_id seq_id = 0;
    int n_past = 0;
    llama_token id_last = 0;
    std::vector<llama_token> prompt_tgt;
    std::vector<llama_token> draft;
    std::string generated;
    int n_generated = 0;
    int n_drafted = 0;
    int n_accepted = 0;
    bool has_eos = false;
    std::string finish_reason;
    entropic_error_t error_code = ENTROPIC_OK;
    std::string error_message;

    // ── Checkpoint state (v2.1.11) ──────────────────────────
    // Activated when either context reports FULL-only seq_rm
    // (no partial removal). The kernel saves+restores draft/target
    // state across each speculative round so the underlying
    // memory module never sees an attempted partial removal.
    // Mirrors the use_ckpt_tgt / use_ckpt_dft flow in upstream's
    // speculative-simple example.
    bool use_ckpt_tgt = false;
    bool use_ckpt_dft = false;
    common_prompt_checkpoint ckpt;
};

/**
 * @brief Free everything allocated by the kernel.
 * @param state Kernel state.
 * @internal
 * @version 2.1.11
 */
static void spec_cleanup(SpeculativeRunState& state) {
    if (state.spec) { common_speculative_free(state.spec); }
    if (state.smpl) { common_sampler_free(state.smpl); }
    if (state.batch_initialized) {
        llama_batch_free(state.batch_tgt);
    }
}

/**
 * @brief Build the target batch [id_last, draft0, ..., draftN-1].
 * @param state Kernel state.
 * @internal
 * @version 2.1.11
 */
static void spec_build_batch(SpeculativeRunState& state) {
    common_batch_clear(state.batch_tgt);
    common_batch_add(state.batch_tgt, state.id_last,
                     state.n_past, {state.seq_id}, true);
    int pos = state.n_past + 1;
    for (auto draft_token : state.draft) {
        common_batch_add(state.batch_tgt, draft_token, pos,
                         {state.seq_id}, true);
        ++pos;
    }
}

/**
 * @brief Decode the speculative batch on both contexts. Populates
 *        state.error_* on failure.
 * @return true on success, false on decode failure.
 * @internal
 * @version 2.1.11 [reviewed]
 */
static bool spec_decode_both(SpeculativeRunState& state) {
    spec_build_batch(state);
    int rc_tgt = llama_decode(state.ctx_tgt, state.batch_tgt);
    if (rc_tgt != 0) {
        logger->error("Speculative target decode failed: rc={}, "
                      "n_past={}, draft_size={}",
                      rc_tgt, state.n_past, state.draft.size());
        state.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        state.error_message = "target llama_decode failed";
        state.finish_reason = "error";
        return false;
    }
    int rc_dft = llama_decode(state.ctx_dft, state.batch_tgt);
    if (rc_dft != 0) {
        logger->error("Speculative draft decode failed: rc={}, "
                      "n_past={}, draft_size={}",
                      rc_dft, state.n_past, state.draft.size());
        state.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        state.error_message = "draft llama_decode failed";
        state.finish_reason = "error";
        return false;
    }
    return true;
}

/**
 * @brief Trigger draft generation via common_speculative_draft.
 * @return Number of draft tokens proposed.
 * @internal
 * @version 2.1.11
 */
static int spec_run_draft(SpeculativeRunState& state) {
    auto& dp = common_speculative_get_draft_params(
        state.spec, state.seq_id);
    dp.drafting = true;
    dp.n_max = -1;
    dp.n_past = state.n_past;
    dp.id_last = state.id_last;
    dp.prompt = &state.prompt_tgt;
    dp.result = &state.draft;
    common_speculative_draft(state.spec);
    return static_cast<int>(state.draft.size());
}

/**
 * @brief Emit on_token for one accepted id, updating state and
 *        returning a stop signal when terminating conditions apply.
 *
 * Outcomes:
 *  - "" continue;
 *  - "eos"     hit end-of-generation token (sets finish_reason="stop")
 *  - "length"  reached max_tokens
 *  - "cancel"  cancel flag set (sets error_code=CANCELLED)
 *
 * @internal
 * @version 2.1.11
 */
static std::string spec_emit_token(
    SpeculativeRunState& state, llama_token id,
    const llama_vocab* vocab, int max_tokens,
    std::function<void(std::string_view)>& on_token,
    std::atomic<bool>& cancel)
{
    std::string signal;
    state.prompt_tgt.push_back(state.id_last);
    state.id_last = id;
    state.n_generated++;
    if (llama_vocab_is_eog(vocab, id)) {
        state.has_eos = true;
        state.finish_reason = "stop";
        signal = "eos";
    } else {
        const std::string piece =
            common_token_to_piece(state.ctx_tgt, id);
        state.generated += piece;
        if (on_token) { on_token(piece); }
        if (cancel.load(std::memory_order_acquire)) {
            state.error_code = ENTROPIC_ERROR_CANCELLED;
            state.finish_reason = "cancelled";
            signal = "cancel";
        } else if (state.n_generated >= max_tokens) {
            state.finish_reason = "length";
            signal = "length";
        }
    }
    return signal;
}

/**
 * @brief Drive one accept round: draft → decode → sample-and-accept
 *        → emit tokens. Returns true to continue the outer loop.
 * @internal
 * @version 2.1.11 [reviewed]
 */
/**
 * @brief Snapshot draft state before drafting (when use_ckpt_dft).
 * @internal
 * @version 2.1.11
 */
static void spec_ckpt_save_dft(SpeculativeRunState& state) {
    state.ckpt.update_pos(
        static_cast<int64_t>(state.prompt_tgt.size()),
        llama_memory_seq_pos_min(
            llama_get_memory(state.ctx_tgt), state.seq_id),
        llama_memory_seq_pos_max(
            llama_get_memory(state.ctx_tgt), state.seq_id));
    if (state.use_ckpt_dft) {
        state.ckpt.update_dft(state.ctx_dft, state.seq_id,
            LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY
                | LLAMA_STATE_SEQ_FLAGS_ON_DEVICE);
    }
}

/**
 * @brief Snapshot target state right before the target decode of
 *        the speculative batch (when use_ckpt_tgt + non-empty draft).
 * @internal
 * @version 2.1.11
 */
static void spec_ckpt_save_tgt(SpeculativeRunState& state) {
    if (state.use_ckpt_tgt && !state.draft.empty()) {
        state.ckpt.update_tgt(state.ctx_tgt, state.seq_id,
            LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY
                | LLAMA_STATE_SEQ_FLAGS_ON_DEVICE);
    }
}

/**
 * @brief Restore the draft's pre-draft state so the upcoming
 *        target-batch decode on the draft re-fills cleanly.
 * @internal
 * @version 2.1.11
 */
static void spec_ckpt_restore_dft(SpeculativeRunState& state) {
    constexpr auto flags = LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY
                         | LLAMA_STATE_SEQ_FLAGS_ON_DEVICE;
    if (state.use_ckpt_dft) {
        state.ckpt.load_dft(state.ctx_dft, state.seq_id, flags);
    }
    llama_memory_seq_rm(llama_get_memory(state.ctx_dft),
                        state.seq_id, state.ckpt.pos_max + 1, -1);
}

/**
 * @brief Partial-acceptance rollback: restore both contexts and
 *        the sampler to their pre-draft state, then arrange for the
 *        outer loop to re-decode with the partial accept as the new
 *        draft. Matches speculative-simple's partial-acceptance
 *        path lines 258-281.
 * @internal
 * @version 2.1.11
 */
static void spec_rollback_partial(
    SpeculativeRunState& state, common_sampler* smpl_save,
    std::vector<llama_token>& ids) {
    constexpr auto flags = LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY
                         | LLAMA_STATE_SEQ_FLAGS_ON_DEVICE;
    state.draft = std::move(ids);
    state.ckpt.load_tgt(state.ctx_tgt, state.seq_id, flags);
    llama_memory_seq_rm(llama_get_memory(state.ctx_tgt),
                        state.seq_id, state.ckpt.pos_max + 1, -1);
    state.ckpt.load_dft(state.ctx_dft, state.seq_id, flags);
    llama_memory_seq_rm(llama_get_memory(state.ctx_dft),
                        state.seq_id, state.ckpt.pos_max + 1, -1);
    state.prompt_tgt.resize(static_cast<size_t>(state.ckpt.n_tokens));
    state.n_past = static_cast<int>(state.prompt_tgt.size());
    // Sampler clone is non-null only when use_ckpt_tgt is set
    common_sampler_free(state.smpl);
    state.smpl = smpl_save;
}

/**
 * @brief Clear any stale KV positions left by rejected draft tokens.
 *
 * After `spec_decode_both` populates positions `[n_past_old,
 * n_past_old + K]` (one for id_last + K draft slots), the sampler
 * accepts a prefix of length `1 + n_accepted` ≤ `1 + K`. n_past is
 * then advanced to the next-write slot = old + 1 + n_accepted.
 * Everything at or beyond `state.n_past` belongs to rejected drafts
 * and must be removed before the next iteration's decode hits the
 * same slots.
 *
 * Matches upstream `speculative-simple.cpp:323`:
 *   `llama_memory_seq_rm(get_memory(ctx), seq_id, n_past, -1)`.
 * The previous `n_past + 1` boundary was off-by-one and left the
 * first rejected-draft slot polluted, causing rc=-1 on PART seq_rm
 * contexts (Gemma 4) on the second iteration (Session 5).
 *
 * PART contexts honour the seq_rm directly; FULL contexts effectively
 * no-op (their KV is restored via the checkpoint dance in
 * `spec_rollback_partial` instead).
 *
 * @internal
 * @version 2.1.11 [reviewed]
 */
static void spec_trim_rejected_drafts(SpeculativeRunState& state) {
    llama_memory_seq_rm(llama_get_memory(state.ctx_tgt),
                        state.seq_id, state.n_past, -1);
    llama_memory_seq_rm(llama_get_memory(state.ctx_dft),
                        state.seq_id, state.n_past, -1);
}

/**
 * @brief Walk accepted ids, emit tokens via callback, update state.
 *        Returns true if the outer loop should stop.
 * @internal
 * @version 2.1.11
 */
static bool spec_commit_accepted(
    SpeculativeRunState& state,
    const std::vector<llama_token>& ids,
    const llama_vocab* vocab, int max_tokens,
    std::function<void(std::string_view)>& on_token,
    std::atomic<bool>& cancel) {
    bool stop = false;
    for (auto id : ids) {
        auto signal = spec_emit_token(
            state, id, vocab, max_tokens, on_token, cancel);
        if (!signal.empty()) { stop = true; break; }
    }
    return stop;
}

/**
 * @brief Drive one accept round: optional draft generation,
 *        decode on both contexts, sample-and-accept, emit tokens
 *        (or roll back via checkpoint on partial acceptance).
 *        Returns true to continue the outer loop.
 * @internal
 * @version 2.1.11 [reviewed]
 */
/**
 * @brief Draft tokens for a speculative round (or reuse carry-over).
 *
 * Extracted from spec_accept_round. When state.draft is empty, runs
 * the draft model under checkpoint save/restore; otherwise reuses the
 * carried-over partial draft.
 *
 * @param state Speculative run state.
 * @return Number of tokens drafted this round.
 * @utility
 * @version 2.3.7
 */
static int spec_prepare_draft(SpeculativeRunState& state) {
    // Skip drafting if the previous round restored a partial accept
    // into state.draft (carry-over from rollback).
    if (!state.draft.empty()) {
        return static_cast<int>(state.draft.size());
    }
    spec_ckpt_save_dft(state);
    int drafted = spec_run_draft(state);
    spec_ckpt_save_tgt(state);
    spec_ckpt_restore_dft(state);
    return drafted;
}

/**
 * @brief Run one speculative accept round; return false to stop.
 * @internal
 * @version 2.3.7
 */
static bool spec_accept_round(
    SpeculativeRunState& state,
    const llama_vocab* vocab,
    int max_tokens,
    std::function<void(std::string_view)>& on_token,
    std::atomic<bool>& cancel)
{
    int draft_size_before = spec_prepare_draft(state);

    if (!spec_decode_both(state)) { return false; }

    common_sampler* smpl_save = nullptr;
    if (state.use_ckpt_tgt) {
        smpl_save = common_sampler_clone(state.smpl);
    }
    auto ids = common_sampler_sample_and_accept_n(
        state.smpl, state.ctx_tgt, state.draft);
    int accepted = static_cast<int>(ids.size()) - 1;
    if (accepted < 0) { accepted = 0; }

    // Partial acceptance on a FULL-seq_rm context: rollback to
    // checkpoint, set draft = accepted, re-loop without emitting.
    if (state.use_ckpt_tgt
        && static_cast<int>(ids.size()) - 1
               < static_cast<int>(state.draft.size())) {
        spec_rollback_partial(state, smpl_save, ids);
        return true;
    }
    if (smpl_save) { common_sampler_free(smpl_save); }

    common_speculative_accept(state.spec, state.seq_id, accepted);
    state.n_drafted += draft_size_before;
    state.n_accepted += accepted;
    // n_past advances by ids.size() total: one slot for id_last
    // (the post-id_last position the next id will occupy), plus
    // `accepted` slots for the drafted tokens the sampler agreed
    // with. Matches speculative-simple's n_past++ in batch_add +
    // n_past += ids.size() - 1 sequence.
    state.n_past += static_cast<int>(ids.size());

    bool stop = spec_commit_accepted(
        state, ids, vocab, max_tokens, on_token, cancel);
    state.draft.clear();
    spec_trim_rejected_drafts(state);
    return !stop;
}

/**
 * @brief Validate speculative preconditions and reject NO-seq_rm.
 *
 * Either FULL or PART seq_rm is acceptable — the kernel has both a
 * partial-seq_rm fast path (PART) and a checkpoint-based path
 * (FULL) that mirrors speculative-simple's use_ckpt branches. Only
 * NO-seq_rm targets/drafts cannot be supported.
 *
 * @return Empty string on success, diagnostic on failure.
 * @internal
 * @version 2.1.11 [reviewed]
 */
static std::string spec_check_preconditions(
    bool target_active, bool draft_active,
    llama_context* ctx_tgt, llama_context* ctx_dft) {
    // Defense-in-depth arch gate — orchestrator's
    // check_speculative_compat is the primary gate; a direct caller
    // into the kernel must also be refused on recurrent / hybrid
    // targets (Session 5 Gate A: hybrid SSM state diverges across
    // split-prefill boundaries; bit-identical unreachable at this pin).
    std::string err;
    const llama_model* model_tgt = llama_get_model(ctx_tgt);
    int cap_tgt = common_context_can_seq_rm(ctx_tgt);
    int cap_dft = common_context_can_seq_rm(ctx_dft);
    logger->info("Speculative seq_rm capability: target={}, draft={} "
                 "(0=NO, 1=PART, 2=FULL)", cap_tgt, cap_dft);
    if (!target_active || !draft_active) {
        err = "speculative requires ACTIVE target + draft";
    } else if (llama_model_is_recurrent(model_tgt)
               || llama_model_is_hybrid(model_tgt)) {
        err = "speculative refused: architecture (target is "
              "recurrent or hybrid; see proposal Implementation "
              "Log Gate A)";
    } else if (cap_tgt == COMMON_CONTEXT_SEQ_RM_TYPE_NO
               || cap_dft == COMMON_CONTEXT_SEQ_RM_TYPE_NO) {
        // NO is the only unsupported seq_rm case — the kernel has
        // both PART fast-path and FULL checkpoint paths.
        err = "speculative kernel requires at least FULL seq_rm "
              "(target/draft reported NO seq_rm at all)";
    }
    return err;
}

/**
 * @brief Initialize the kernel state: clear KV, prefill, sampler,
 *        speculative context, batch, and detect FULL-seq_rm
 *        checkpoint-mode for target/draft. Returns "" on success or
 *        a diagnostic string. Cleans up on failure.
 *
 * Checkpoint detection runs AFTER `common_speculative_begin` so the
 * impl's batch is already allocated.
 *
 * @internal
 * @version 2.1.11
 */
/**
 * @brief Init the sampler + speculative decoder for a run.
 *
 * Extracted from spec_init_run to keep it knots-clean. Returns "" on
 * success after wiring state.smpl/spec/batch + checkpoint flags;
 * returns a diagnostic and cleans up on failure.
 *
 * @param state Speculative run state.
 * @param model_tgt Target model (for the sampler).
 * @param params Generation params.
 * @param n_draft_max Draft cap (<=0 → 16).
 * @param draft_path Draft model path (gates DRAFT_SIMPLE upstream).
 * @return Empty on success, diagnostic on failure.
 * @utility
 * @version 2.3.7
 */
static std::string spec_init_sampler_and_decoder(
    SpeculativeRunState& state, llama_model* model_tgt,
    const GenerationParams& params, int n_draft_max,
    const std::string& draft_path) {
    auto common_sampling = to_common_sampling(params);
    state.smpl = common_sampler_init(model_tgt, common_sampling);
    if (!state.smpl) { return "common_sampler_init failed"; }

    common_params_speculative spec_params;
    spec_params.types = {COMMON_SPECULATIVE_TYPE_DRAFT_SIMPLE};
    spec_params.draft.n_max = (n_draft_max > 0) ? n_draft_max : 16;
    spec_params.draft.ctx_tgt = state.ctx_tgt;
    spec_params.draft.ctx_dft = state.ctx_dft;
    // Upstream gates DRAFT_SIMPLE on a non-empty draft path
    // (see common/speculative.cpp:875). Required even though we
    // provide already-loaded contexts.
    spec_params.draft.mparams.path = draft_path;
    state.spec = common_speculative_init(spec_params, 1);
    if (!state.spec) {
        common_sampler_free(state.smpl);
        state.smpl = nullptr;
        return "common_speculative_init failed";
    }

    common_speculative_begin(state.spec, state.seq_id, state.prompt_tgt);
    state.batch_tgt = llama_batch_init(llama_n_batch(state.ctx_tgt), 0, 1);
    state.batch_initialized = true;
    // Checkpoint flow lights up when either context can only do
    // FULL-sequence removal. Mirrors speculative-simple's
    // use_ckpt_{tgt,dft}.
    state.use_ckpt_tgt = common_context_can_seq_rm(state.ctx_tgt)
        == COMMON_CONTEXT_SEQ_RM_TYPE_FULL;
    state.use_ckpt_dft = common_context_can_seq_rm(state.ctx_dft)
        == COMMON_CONTEXT_SEQ_RM_TYPE_FULL;
    return "";
}

/**
 * @brief Initialize speculative run state (prefill + sampler + decoder).
 * @internal
 * @version 2.3.7
 */
static std::string spec_init_run(
    SpeculativeRunState& state, llama_model* model_tgt,
    const std::vector<llama_token>& tokens,
    const GenerationParams& params, int n_draft_max,
    const std::string& draft_path) {
    state.id_last = tokens.back();
    state.prompt_tgt.assign(tokens.begin(), tokens.end() - 1);
    state.n_past = static_cast<int>(tokens.size()) - 1;

    llama_memory_clear(llama_get_memory(state.ctx_tgt), true);
    llama_memory_clear(llama_get_memory(state.ctx_dft), true);

    if (!spec_prefill_minus_last(state.ctx_tgt, tokens)
        || !spec_prefill_minus_last(state.ctx_dft, tokens)) {
        return "speculative prefill failed";
    }
    return spec_init_sampler_and_decoder(
        state, model_tgt, params, n_draft_max, draft_path);
}

/**
 * @brief Run the accept-round loop until completion / EOS / cancel.
 * @internal
 * @version 2.1.11
 */
static void spec_run_loop(
    SpeculativeRunState& state, const llama_vocab* vocab,
    int max_tokens,
    std::function<void(std::string_view)>& on_token,
    std::atomic<bool>& cancel) {
    while (state.n_generated < max_tokens) {
        if (cancel.load(std::memory_order_acquire)) {
            state.error_code = ENTROPIC_ERROR_CANCELLED;
            state.finish_reason = "cancelled";
            break;
        }
        if (!spec_accept_round(state, vocab, max_tokens,
                               on_token, cancel)) {
            break;
        }
    }
    if (state.finish_reason.empty()) {
        state.finish_reason = (state.n_generated >= max_tokens)
                                  ? "length" : "stop";
    }
}

/**
 * @brief Speculative kernel against an explicit draft backend.
 * @internal
 * @version 2.1.11
 */
/**
 * @brief Assemble final GenerationResult + log metrics. Helper to
 *        keep the public entry under SLOC ≤ 50.
 * @internal
 * @version 2.1.11
 */
static GenerationResult spec_finalize(
    SpeculativeRunState& state,
    std::chrono::steady_clock::time_point t0) {
    GenerationResult result;
    result.content = state.generated;
    result.token_count = state.n_generated;
    result.finish_reason = state.finish_reason;
    result.error_code = state.error_code;
    result.error_message = state.error_message;
    result.generation_time_ms =
        entropic::log::elapsed_ms(t0, entropic::log::now());
    if (state.n_drafted > 0) {
        const float accept_rate =
            static_cast<float>(state.n_accepted)
                / static_cast<float>(state.n_drafted);
        logger->info("Speculative: generated={}, drafted={}, "
                     "accepted={}, accept_rate={:.3f}",
                     state.n_generated, state.n_drafted,
                     state.n_accepted, accept_rate);
    }
    spec_cleanup(state);
    return result;
}

/**
 * @brief Public entry point for the speculative-decoding kernel.
 *
 * Validates preconditions, tokenizes the prompt, initializes kernel
 * state, drives the accept-round loop, and finalizes the result.
 * Each phase is delegated to a static helper to satisfy the
 * complexity gates (knots: cognitive ≤ 15, SLOC ≤ 50, returns ≤ 3).
 *
 * @param messages Conversation history.
 * @param params Generation parameters (samplers, max_tokens, seed).
 * @param on_token Callback fired once per accepted token.
 * @param cancel Cancellation flag (polled between accept rounds).
 * @param draft Draft backend (must be ACTIVE on the same model arch).
 * @param n_draft_max Maximum draft window size (proposed tokens per
 *        round). Clamped to 16 if non-positive.
 * @return GenerationResult.
 * @internal
 * @version 2.1.11 [reviewed]
 */
/**
 * @brief Run the speculative loop over already-tokenized input.
 *
 * Extracted from generate_speculative_with_draft to keep it
 * knots-clean. Inits the run state, drives the accept-round loop, and
 * finalizes; cleans up + returns a typed error if init fails.
 *
 * @param ctx_tgt Target context.
 * @param ctx_dft Draft context.
 * @param model_tgt Target model (sampler + vocab).
 * @param tokens Prompt tokens (>= 2).
 * @param params Generation parameters.
 * @param on_token Per-accepted-token callback.
 * @param cancel Cancellation flag.
 * @param n_draft_max Draft window cap.
 * @param draft_path Draft model path.
 * @param t0 Start timestamp for timing.
 * @return GenerationResult.
 * @utility
 * @version 2.3.7
 */
static GenerationResult spec_run_from_tokens(
    llama_context* ctx_tgt, llama_context* ctx_dft, llama_model* model_tgt,
    const std::vector<llama_token>& tokens, const GenerationParams& params,
    std::function<void(std::string_view)>& on_token,
    std::atomic<bool>& cancel, int n_draft_max,
    const std::string& draft_path,
    std::chrono::steady_clock::time_point t0) {
    SpeculativeRunState state;
    state.ctx_tgt = ctx_tgt;
    state.ctx_dft = ctx_dft;
    auto init_err = spec_init_run(state, model_tgt, tokens, params,
                                  n_draft_max, draft_path);
    if (!init_err.empty()) {
        spec_cleanup(state);
        return spec_error(ENTROPIC_ERROR_GENERATE_FAILED,
                          std::move(init_err));
    }
    spec_run_loop(state, llama_model_get_vocab(model_tgt),
                  params.max_tokens, on_token, cancel);
    return spec_finalize(state, t0);
}

/**
 * @brief Speculative generation against a draft model (gh#36).
 * @internal
 * @version 2.7.5
 */
GenerationResult LlamaCppBackend::generate_speculative_with_draft(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view)> on_token,
    std::atomic<bool>& cancel,
    LlamaCppBackend& draft,
    int n_draft_max,
    const std::string& draft_path)
{
    auto t0 = entropic::log::now();
    invalidate_resident_kv();  // gh#96: speculative path manages seq 0 itself
    auto pre_err = spec_check_preconditions(
        is_active(), draft.is_active(), ctx_, draft.ctx_);
    GenerationResult result;
    if (!pre_err.empty()) {
        entropic_error_t code =
            (pre_err.find("requires ACTIVE") != std::string::npos)
                ? ENTROPIC_ERROR_INVALID_STATE
                : ENTROPIC_ERROR_NOT_SUPPORTED;
        result = spec_error(code, std::move(pre_err));
    } else {
        auto prompt = render_prompt(messages, params);
        auto tokens = tokenize(prompt, true);
        if (tokens.size() < 2) {
            result = spec_error(ENTROPIC_ERROR_GENERATE_FAILED,
                "speculative prompt must have at least 2 tokens");
        } else {
            logger->info("Speculative: {} input tokens, max_tokens={}, "
                         "n_draft_max={}",
                         tokens.size(), params.max_tokens, n_draft_max);
            result = spec_run_from_tokens(
                ctx_, draft.ctx_, model_, tokens, params, on_token,
                cancel, n_draft_max, draft_path, t0);
        }
    }
    return result;
}

/**
 * @brief Raw text completion without chat template.
 * @param prompt Raw prompt string.
 * @param params Generation parameters.
 * @return GenerationResult.
 * @internal
 * @version 2.7.5
 */
GenerationResult LlamaCppBackend::do_complete(
    const std::string& prompt,
    const GenerationParams& params)
{
    auto t0 = entropic::log::now();
    invalidate_resident_kv();  // gh#96: decode_loop/run_prefill mutate seq 0
    auto tokens = tokenize(prompt, false);

    logger->info("Complete: {} input tokens, max_tokens={}",
              tokens.size(), params.max_tokens);
    log_sampler_config(params);
    auto result = decode_loop(tokens, params, nullptr, nullptr);
    finalize_result(result, t0);
    return result;
}

// ── Architecture detection (v1.9.13) ───────────────────────

/**
 * @brief Check if loaded model is recurrent.
 * @return true if GDN/Mamba/RWKV architecture.
 * @internal
 * @version 1.9.13
 */
bool LlamaCppBackend::is_recurrent() const {
    return is_recurrent_;
}

// ── Capability overrides (v1.9.13) ─────────────────────────

/**
 * @brief Declare llama.cpp backend capabilities.
 * @param cap Capability to check.
 * @return true if this backend supports the capability.
 * @internal
 * @version 1.9.13
 */
bool LlamaCppBackend::do_supports(BackendCapability cap) const {
    int idx = static_cast<int>(cap);
    int count = static_cast<int>(BackendCapability::_COUNT);
    if (idx < 0 || idx >= count) {
        return false;
    }

    // Static capabilities: true = always supported. Length must equal
    // BackendCapability::_COUNT — trailing entries get appended as new
    // capabilities are introduced (gh#53 added AUDIO at index 12).
    static constexpr bool always[] = {
        false, false, true, true, true, true,
        false, true,  true, false, false, true,
        false,  // AUDIO — dynamic only (mtmd_support_audio)
    };

    // Dynamic capabilities override the static table
    bool result = always[idx];
    if (!result) {
        result = (cap == BackendCapability::KV_CACHE && !is_recurrent())
              || (cap == BackendCapability::HIDDEN_STATE && is_recurrent())
              || (cap == BackendCapability::VISION
                  && !config().mmproj_path.empty())
              || (cap == BackendCapability::AUDIO
                  && mtmd_ctx_ != nullptr
                  && mtmd_support_audio(mtmd_ctx_))
              || (cap == BackendCapability::SPECULATIVE_DECODING
                  && !is_recurrent());
    }
    return result;
}

/**
 * @brief Return backend name.
 * @return "llama.cpp".
 * @internal
 * @version 1.9.13
 */
std::string LlamaCppBackend::do_backend_name() const {
    return "llama.cpp";
}

/**
 * @brief Populate backend metadata from llama.cpp model.
 * @return BackendInfo with model-specific details.
 * @internal
 * @version 1.9.13
 */
BackendInfo LlamaCppBackend::do_info() const {
    BackendInfo bi;
    bi.name = "llama.cpp";
#if defined(ENTROPIC_BACKEND_CUDA)
    bi.compute_device = "cuda";
#elif defined(ENTROPIC_BACKEND_VULKAN)
    bi.compute_device = "vulkan";
#else
    bi.compute_device = "cpu";
#endif
    bi.model_format = "gguf";

    if (state() != ModelState::COLD && model_ != nullptr) {
        bi.architecture = is_recurrent() ? "recurrent" : "transformer";
        bi.max_context_length =
            is_recurrent() ? -1 : config().context_length;
        bi.parameter_count = llama_model_n_params(model_);
        bi.vram_bytes = 0;
        bi.ram_bytes = llama_model_size(model_);

        char desc[256] = {};
        llama_model_desc(model_, desc, sizeof(desc));
        bi.quantization = desc;
    }
    return bi;
}

/**
 * @brief Clear KV cache or recurrent hidden state.
 * @param seq_id Sequence ID, or -1 for all sequences.
 * @return true on success.
 * @internal
 * @version 1.9.13
 */
bool LlamaCppBackend::do_clear_state(int seq_id) {
    if (ctx_ == nullptr) {
        return false;
    }
    auto mem = llama_get_memory(ctx_);
    if (seq_id < 0) {
        llama_memory_clear(mem, true);
    } else {
        llama_memory_seq_rm(mem, seq_id, -1, -1);
    }
    return true;
}

/**
 * @brief Capture one sequence's KV cache via llama_state_seq_get_data.
 *
 * gh#23 MVP item 13 (v2.3.25 + v2.4.0). v2.3.25 shipped the C API
 * surface (`entropic_state_save` / `entropic_state_load`) but the
 * backend layer was never overridden — the base-class stub returned
 * false, so the C API always reported `ENTROPIC_ERROR_INTERNAL`
 * against a real model. The v2.4.0 minor-release ceremony surfaced
 * this; the fix wires llama.cpp's sequence-scoped state API here.
 *
 * @param seq_id llama sequence id (the C API path passes 0).
 * @param buffer Output; resized to the exact state size.
 * @return true when llama.cpp emitted the full sized blob.
 * @internal
 * @version 2.4.0
 */
bool LlamaCppBackend::do_save_state(
    int seq_id, std::vector<uint8_t>& buffer) const {
    if (ctx_ == nullptr) { return false; }
    size_t sz = llama_state_seq_get_size(
        ctx_, static_cast<llama_seq_id>(seq_id));
    if (sz == 0) { return false; }
    buffer.resize(sz);
    size_t written = llama_state_seq_get_data(
        ctx_, buffer.data(), sz,
        static_cast<llama_seq_id>(seq_id));
    return written == sz;
}

/**
 * @brief Restore one sequence's KV cache via llama_state_seq_set_data.
 *
 * gh#23 MVP item 13 (v2.4.0 follow-up to v2.3.25).
 *
 * @param seq_id llama sequence id.
 * @param buffer Bytes captured by a prior `do_save_state` against the
 *               SAME model + same llama.cpp commit pin.
 * @return true when llama_state_seq_set_data reports a non-zero
 *         accepted size (per the llama.cpp contract: positive=ok,
 *         zero=failed to load).
 * @internal
 * @version 2.4.0
 */
bool LlamaCppBackend::do_restore_state(
    int seq_id, const std::vector<uint8_t>& buffer) {
    if (ctx_ == nullptr || buffer.empty()) { return false; }
    size_t result = llama_state_seq_set_data(
        ctx_, buffer.data(), buffer.size(),
        static_cast<llama_seq_id>(seq_id));
    return result > 0;
}

} // namespace entropic
