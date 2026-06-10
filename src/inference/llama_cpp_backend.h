// SPDX-License-Identifier: Apache-2.0
/**
 * @file llama_cpp_backend.h
 * @brief LlamaCppBackend — llama.cpp C API integration.
 *
 * Versioned subclass pattern: LlamaCppBackend provides common llama.cpp
 * patterns (decode loop, sampler chain, tokenization). The pinned-commit
 * subclass (LlamaCppBackend_b8420) overrides API-version-specific calls.
 *
 * @par VRAM lifecycle mapping
 * - COLD: nothing allocated
 * - WARM: llama_model loaded (CPU mmap+mlock, n_gpu_layers=0)
 * - ACTIVE: model reloaded with gpu_layers, llama_context created
 *
 * @par Key differences from Python LlamaCppBackend
 * - Direct llama.cpp C API (not llama-cpp-python wrapper)
 * - No Python GIL — generation runs natively
 * - No asyncio bridge — streaming is synchronous with callback
 *
 * Internal to inference .so — not exposed across boundaries.
 *
 * @version 1.9.13
 */

#pragma once

#include <entropic/inference/backend.h>
#include <entropic/inference/sampler.h>
#include <entropic/inference/tokenizer.h>

#include "prompt_cache.h"

#include <llama.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward-declare libmtmd's opaque types at file scope so they
// resolve to ::mtmd_context / ::mtmd_bitmap (not entropic::mtmd_*)
// when referenced inside the class body. Full types live in
// extern/llama.cpp/tools/mtmd/mtmd.h and are only included from
// the implementation file. (v2.1.8, gh#37/v1.9.11 Phase 5)
extern "C" {
struct mtmd_context;
struct mtmd_bitmap;
}

namespace entropic {

/**
 * @brief LlamaCppBackend — common llama.cpp patterns (15% layer).
 *
 * Provides decode loop, sampler chain creation, tokenization helpers.
 * Pinned-version subclass overrides do_load/do_activate with
 * version-specific API calls.
 *
 * @version 1.8.3
 */
class LlamaCppBackend : public InferenceBackend {
public:
    /**
     * @brief Free llama.cpp + mtmd resources on destruction.
     *
     * gh#58 v2.2.7 follow-up: the base `InferenceBackend` destructor
     * is defaulted, so without this override the raw `model_` / `ctx_`
     * / `mtmd_ctx_` pointers leak when the backend goes out of scope.
     * GPU buffers held by those objects stay allocated for the
     * remainder of the process, and the next handle's GPU model load
     * fails because llama.cpp's CUDA pool sees the prior allocations
     * as stale-but-occupied.
     *
     * @utility
     * @version 2.2.8
     */
    ~LlamaCppBackend() override;

    /**
     * @brief Inject a tokenizer for unit testing (v2.3.10).
     *
     * Bypasses do_load() to wire a mock Tokenizer into the backend
     * AND mark the backend as WARM so is_loaded()-gated public
     * methods (tokenize_text, do_count_tokens via base count_tokens)
     * route through the mock. Production code MUST NOT call this —
     * use load() / activate() for real model lifecycle.
     *
     * Distinct entrypoint name + no production caller ⇒ no risk of
     * accidental misuse; `#ifdef ENTROPIC_TESTING` was considered
     * but rejected (the build-flag conditional creates a
     * test-vs-production code-shape divergence we don't want for
     * coverage measurement).
     *
     * @param tokenizer Mock tokenizer (ownership transferred).
     * @utility
     * @version 2.3.10
     */
    void inject_tokenizer_for_test(std::unique_ptr<Tokenizer> tokenizer);

    /**
     * @brief Inject a SamplerFactory for unit testing (v2.3.10).
     *
     * Bypasses do_activate() to wire a mock factory into the backend
     * so the decode loop's chain-construction + per-token sampling
     * paths can be exercised without a real `llama_context`. Production
     * code MUST NOT call this — use activate() for real lifecycle.
     *
     * Symmetry: matches `inject_tokenizer_for_test` in shape so the
     * two v2.3.10 seams (Tokenizer + Sampler) feel consistent at the
     * test surface. No `#ifdef ENTROPIC_TESTING` — the build-flag
     * conditional creates a test-vs-production code-shape divergence
     * we don't want for coverage measurement.
     *
     * @param factory Mock factory (ownership transferred).
     * @utility
     * @version 2.3.10
     */
    void inject_sampler_factory_for_test(
        std::unique_ptr<SamplerFactory> factory);

    /**
     * @brief Read the currently-wired SamplerFactory (test-only).
     *
     * Returns nullptr when no factory has been wired yet (the COLD
     * state of `sampler_factory_`). Exists so seam tests can assert
     * factory presence without poking at private members.
     *
     * @return Borrowed factory pointer (do not free); nullptr if unset.
     * @utility
     * @version 2.3.10
     */
    SamplerFactory* sampler_factory_for_test() const {
        return sampler_factory_.get();
    }

    /**
     * @brief Allocate a temp seq_id (test-only seam for gh#98).
     *
     * Exposes allocate_temp_seq_id so a CPU unit test can assert the pool hands
     * out DISTINCT ids on an empty pool — the invariant the gh#98 multi-seq
     * batch relies on (the model test can't catch a collision because a forcing
     * grammar makes output independent of the corrupted KV).
     * @return A temp seq_id.
     * @utility
     * @version 2.8.0
     */
    llama_seq_id allocate_temp_seq_id_for_test() {
        return allocate_temp_seq_id();
    }

    /**
     * @brief Release a temp seq_id (test-only seam, gh#98).
     * @param id The seq_id to release.
     * @utility
     * @version 2.8.0
     */
    void release_temp_seq_id_for_test(llama_seq_id id) {
        release_temp_seq_id(id);
    }

    /**
     * @brief Set prompt cache configuration.
     *
     * Must be called before activate(). The config is consumed when
     * the cache is constructed during do_activate().
     *
     * @param config Prompt cache configuration.
     * @utility
     * @version 1.8.3
     */
    void set_prompt_cache_config(const PromptCacheConfig& config) {
        prompt_cache_config_ = config;
    }

    /**
     * @brief Drop every cached prefix so the next prefill re-seeds.
     *
     * Called by the orchestrator on identity/prompt-prefix changes.
     * No-op when the cache has not been constructed yet.
     * (P1-7, 2.0.6-rc16)
     *
     * @utility
     * @version 2.0.6-rc16
     */
    void clear_prompt_cache() override {
        if (prompt_cache_) { prompt_cache_->clear(); }
    }

    /**
     * @brief Tokenize text to token IDs using model vocabulary.
     * @param text Input text.
     * @return Token ID vector with BOS.
     * @version 1.10.2
     */
    std::vector<int32_t> tokenize_text(
        const std::string& text) const override;

    /* ── llama.cpp handle accessors (v1.9.2) ────────────── */

    /**
     * @brief Get the loaded llama_model pointer.
     * @return nullptr if state is COLD.
     * @utility
     * @version 1.9.2
     */
    llama_model* llama_model_ptr() { return model_; }

    /**
     * @brief Get the active llama_context pointer.
     * @return nullptr if state is not ACTIVE.
     * @utility
     * @version 1.9.2
     */
    llama_context* llama_context_ptr() { return ctx_; }

    /**
     * @brief Prompt (prefill) tokens actually decoded by the last generation.
     *
     * gh#96 (v2.7.5): counted directly as tokens pushed through llama_decode
     * during prefill (run_prefill + decode_tokens_from accumulate into it;
     * reset per generation in run_prefill_cached). A prompt-cache HIT restores
     * the system prefix without a decode, so this is the re-decoded
     * post-system remainder — in a multi-turn loop it climbs every turn today,
     * and should collapse to the per-turn delta once warm-keep reuse lands.
     * Exposed for the gh#96 behavioral tests to assert reuse without
     * log-scraping. (Counted directly rather than via llama_perf n_p_eval,
     * which proved unreliable across the state-restore boundary.)
     * @return Prompt tokens decoded in the most recent generate() call.
     * @utility
     * @version 2.7.5
     */
    int last_prefill_tokens() const { return last_prefill_tokens_; }

    /**
     * @brief Number of batched generation decodes in the last gh#98 batch.
     *
     * Observable that the multi-seq batched-decode engaged: ≈ the longest
     * per-request output (one decode per step over all sequences), NOT the
     * serial `Σ output_len`.
     * @utility
     * @version 2.8.0
     */
    int last_gen_decode_calls() const { return last_gen_decode_calls_; }

    /**
     * @brief Wall-clock milliseconds spent in prefill by the last generation.
     *
     * gh#96 (v2.7.5): steady_clock around the prefill dispatch in
     * run_prefill_cached — the real per-turn cost the optimization targets
     * (token count is the deterministic proxy; this is the wall-clock the
     * consumer actually feels). Climbs with history today; should flatten
     * once warm-keep reuse lands. The realized magnitude scales with model
     * size + context length (small on a tiny model, large on the consumer's
     * MoE at long context). Measured directly, not via llama_perf.
     * @return Prefill wall-clock ms for the most recent generate() call.
     * @utility
     * @version 2.7.5
     */
    double last_prefill_ms() const { return last_prefill_ms_; }

    /**
     * @brief Tokenized prompt size of the last generation (input tokens).
     * @return Input token count of the most recent generate() call.
     * @utility
     * @version 2.7.6
     */
    int last_input_tokens() const { return last_input_tokens_; }

    /**
     * @brief Highest occupied KV position in seq 0 right now (live query).
     *
     * gh#97 (v2.7.6): a correct prefill leaves exactly `input_tokens` positions
     * resident, so after a generation this is ~`input + generated - 1`. If a
     * prefix-reuse path (warm-keep / cache restore) leaves an un-removed tail
     * — e.g. partial `seq_rm` rejected by recurrent/hybrid memory — this
     * INFLATES beyond the prompt size and eventually exhausts the context with
     * the cache mostly empty. Exposed so the gh#97 hybrid test can assert no
     * inflation, catching the desync immediately instead of at exhaustion.
     * @return seq-0 pos_max, or -1 if not active.
     * @utility
     * @version 2.7.6
     */
    int kv_pos_max() const {
        return ctx_ != nullptr
            ? static_cast<int>(llama_memory_seq_pos_max(llama_get_memory(ctx_), 0))
            : -1;
    }

    /* ── gh#87 (v2.7.0): common_chat tool-call render + parse ── */

    /**
     * @brief Result of a common_chat parse: native tool calls + split content.
     *
     * Mirrors the adapter `ParseResult` (cleaned content + calls) and adds
     * the reasoning split that `common_chat_parse` produces natively, so the
     * orchestrator no longer has to `strip_think_blocks` for covered families.
     *
     * @version 2.7.0
     */
    struct CommonChatResult {
        std::vector<ToolCall> tool_calls;  ///< Extracted native tool calls
        std::string content;               ///< Content with calls + reasoning removed
        std::string reasoning_content;     ///< Extracted reasoning/thought block
    };

    /**
     * @brief Stage tool definitions for the next common_chat render (gh#87).
     *
     * Accepts entropic's MCP tool-list JSON (array of
     * `{name, description, inputSchema}`), as produced by
     * `ServerManager::list_tools()` and filtered per-tier. The defs flow into
     * `common_chat`'s `inputs.tools` at render time so the model is instructed
     * in — and emits — its native tool-call wire format. Pass `""` or `"[]"`
     * to clear.
     *
     * @param tools_json JSON array of MCP tool definitions (borrowed).
     * @utility
     * @version 2.7.0
     */
    void set_active_tools(const std::string& tools_json);

    /**
     * @brief Render messages through common_chat WITH the active tools.
     *
     * Like `apply_chat_template` but routes the staged tool defs into
     * `inputs.tools` and CAPTURES the rendered `common_chat_params` (format,
     * generation_prompt, serialized PEG parser) so a subsequent
     * `parse_response` can decode the model's emission in the same context
     * the render established. Falls back to the low-level template (no
     * capture) if the jinja path is unavailable.
     *
     * @param messages Conversation history.
     * @param params Generation parameters (enable_thinking honored).
     * @return Formatted prompt string.
     * @internal
     * @version 2.7.0
     */
    std::string render_with_tools(
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /**
     * @brief Parse a raw model emission via the last captured render params.
     *
     * Reconstructs the `common_chat_parser_params` from the state captured by
     * the most recent `render_with_tools` and runs `common_chat_parse`. The
     * converting ctor copies only format + generation_prompt, so this
     * explicitly `load()`s the serialized PEG arena — without it the parser
     * silently degrades to pure content and extracts zero tool calls (gh#87
     * Increment-1 finding). If no render has captured params, returns the
     * raw text as content with no calls.
     *
     * @param raw Raw model output (assistant turn only, no generation prompt).
     * @return Parsed tool calls + cleaned content + reasoning.
     * @internal
     * @version 2.7.0
     */
    CommonChatResult parse_response(const std::string& raw) const;

    /**
     * @brief True iff the last render captured common_chat parse params (gh#87).
     *
     * Set when `render_with_tools` (via the internal render seam) renders with
     * active tools; cleared on a tool-less render. The orchestrator queries
     * this to route post-generation parsing through `parse_response` vs the
     * legacy adapter `parse_tool_calls`.
     *
     * @return true if a tool render captured params for `parse_response`.
     * @utility
     * @version 2.7.0
     */
    bool has_common_chat_params() const { return have_chat_params_; }

    /**
     * @brief True iff common_chat parsing is reliable for the last render (gh#87).
     *
     * common_chat's PEG *autoparser* (PEG_NATIVE / PEG_SIMPLE — inferred from
     * a template) only extracts the FIRST `<parameter=>` of a multi-parameter
     * tool call. Only the DEDICATED grammars (currently PEG_GEMMA4) parse
     * multi-parameter calls correctly. So the orchestrator routes parsing to
     * `parse_response` ONLY when the captured format is dedicated; autoparser
     * families fall back to their hand-rolled multi-parameter adapter.
     *
     * Extend the dedicated-format set here as llama.cpp gains hand-written
     * parsers for more families.
     *
     * @return true if `parse_response` is multi-parameter safe for this render.
     * @utility
     * @version 2.7.0
     */
    bool common_chat_parse_reliable() const;

    /**
     * @brief Tool-call close marker for the captured chat format (gh#103).
     *
     * Maps the last render's `last_chat_format_` to the string that terminates
     * a single tool call in that family's native emission, so the orchestrator
     * can append it to GenerationParams.stop for "sequential" tiers (hard-stop
     * at the first closed tool call). Returns "" when no tool render captured
     * params or the format has no reliable marker → caller injects no stop
     * (batch behavior). Markers track the vendored PEG parser defaults
     * (chat-peg-parser.cpp) and are validated empirically by the gh#103 model
     * test; CONTENT_ONLY/unknown → "".
     *
     * @return Close marker for the active format, or "".
     * @utility
     * @version 2.8.2
     */
    std::string tool_call_close_marker() const override;

    /**
     * @brief params.stop + the sequential tool-call close marker, if applicable.
     *
     * gh#105 (v2.8.3): the gh#103 sequential hard-stop must be injected with
     * THIS generation's family marker, derived AFTER render captured the format.
     * Each decode body calls this post-render and feeds the result to step_token
     * — so the marker matches the current render (gh#103 injected it pre-render
     * off the previous/empty format, so it never fired on the first call). Gated
     * on params.tool_call_mode == "sequential"; otherwise returns params.stop
     * unchanged (batch is byte-identical).
     *
     * @param params Generation params (read-only).
     * @return Effective stop-sequence list for the decode loop.
     * @utility
     * @version 2.8.3
     */
    std::vector<std::string> effective_stop(
        const GenerationParams& params) const;

protected:
    /* ── Lifecycle overrides ─────────────────────────────── */

    bool do_load(const ModelConfig& config) override;
    bool do_activate() override;
    void do_deactivate() override;
    void do_unload() override;

    /* ── Generation overrides ────────────────────────────── */

    GenerationResult do_generate(
        const std::vector<Message>& messages,
        const GenerationParams& params) override;

    /**
     * @brief Batch generate with per-token cancel poll. (gh#81, v2.4.2)
     * @version 2.4.2
     */
    GenerationResult do_generate(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::atomic<bool>& cancel) override;

    GenerationResult do_generate_streaming(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel) override;

    /**
     * @brief Speculative streaming via the abstract InferenceBackend
     *        interface (kept as NOT_SUPPORTED — see kernel entry below).
     *
     * The actual draft-pair-aware kernel lives in
     * `generate_speculative_with_draft` and is called by the
     * orchestrator after it has resolved the draft backend. This
     * abstract override exists for backends with implicit draft
     * resolution; LlamaCppBackend requires an explicit draft handle.
     *
     * @return GenerationResult with NOT_SUPPORTED.
     * @version 2.1.11
     */
    GenerationResult do_generate_speculative(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel) override;

    /**
     * @brief Same-prefix batch generation (gh#98, v2.8.0).
     *
     * On a plain-KV arch with a real shared prefix and enough sequence slots
     * (batch_is_viable), prefills the shared prefix once into seq 0 and runs
     * each request's suffix + generation off that prefix, seq_rm-resetting the
     * tail between requests — the proven warm-keep mechanism, applied N times.
     * Otherwise falls back to the serial base implementation. Each request is
     * sampled under its own grammar (option A).
     *
     * @version 2.8.0
     */
    std::vector<GenerationResult> do_generate_batch(
        const std::vector<std::vector<Message>>& requests,
        const std::vector<GenerationParams>& params,
        std::atomic<bool>& cancel) override;

public:
    /**
     * @brief Speculative-decoding kernel with explicit draft backend.
     *
     * Adapts the upstream `speculative-simple` reference loop at pin
     * `253ba110b` to entropic's idioms: drives a draft LlamaCppBackend
     * through `common_speculative_*`, verifies in batch on the target,
     * and emits one `on_token` callback per accepted token (not per
     * proposed) — preserving the standard streaming contract. Honors
     * `cancel` between accept rounds; latency is one accept round
     * (typically 1–8 tokens).
     *
     * Correctness contract: output distribution bit-identical to plain
     * decode on rejection cases. Verified by
     * `test_speculative_correctness.cpp` against Qwen3.6-A3B target
     * + Qwen3.5-0.8B draft.
     *
     * Constraints (v2.1.11):
     *   - Both target and draft must report
     *     `common_context_can_seq_rm == COMMON_CONTEXT_SEQ_RM_TYPE_FULL`.
     *     Falls back to NOT_SUPPORTED otherwise (partial-acceptance
     *     checkpoint restore is deferred — see decision log #41).
     *   - Both backends must be ACTIVE.
     *   - Caller (orchestrator) is responsible for compat verification
     *     before calling — this entry trusts the pair.
     *
     * @param messages Conversation history.
     * @param params Generation parameters (samplers + max_tokens + seed).
     * @param on_token Callback fired once per accepted token.
     * @param cancel Cancellation flag (polled between accept rounds).
     * @param draft Draft backend (must be ACTIVE).
     * @return GenerationResult with content, token_count, finish_reason.
     * @version 2.1.11
     */
    GenerationResult generate_speculative_with_draft(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel,
        LlamaCppBackend& draft,
        int n_draft_max,
        const std::string& draft_path);

protected:
    GenerationResult do_complete(
        const std::string& prompt,
        const GenerationParams& params) override;

    int do_count_tokens(const std::string& text) const override;

    /* ── Evaluation override (v1.9.10) ──────────────────── */

    LogprobResult do_evaluate_logprobs(
        const int32_t* tokens,
        int n_tokens) override;

    /* ── Capability overrides (v1.9.13) ──────────────────── */

    bool do_supports(BackendCapability cap) const override;
    std::string do_backend_name() const override;
    BackendInfo do_info() const override;
    bool do_clear_state(int seq_id) override;

    /* ── State save/load override (gh#23 MVP item 13, v2.3.25) ── */

    /**
     * @brief Capture a sequence's KV cache into a byte buffer.
     *
     * Wraps llama.cpp's `llama_state_seq_get_size` +
     * `llama_state_seq_get_data`. Required by the v2.3.25
     * `entropic_state_save` C API; the base class default returns
     * false (no state support).
     *
     * @param seq_id Sequence id (default 0 from the C API path).
     * @param buffer Output buffer; resized to exact state size.
     * @return true on success; false when not active or copy short-reads.
     * @internal
     * @version 2.4.0
     */
    bool do_save_state(int seq_id,
                       std::vector<uint8_t>& buffer) const override;

    /**
     * @brief Restore a sequence's KV cache from a byte buffer.
     *
     * Wraps llama.cpp's `llama_state_seq_set_data`. Required by the
     * v2.3.25 `entropic_state_load` C API.
     *
     * @param seq_id Sequence id.
     * @param buffer Source buffer (output of a prior save_state).
     * @return true when llama_state_seq_set_data accepts the buffer.
     * @internal
     * @version 2.4.0
     */
    bool do_restore_state(int seq_id,
                          const std::vector<uint8_t>& buffer) override;

    /* ── llama.cpp handles ───────────────────────────────── */

    llama_model* model_ = nullptr;             ///< Loaded model (WARM+)
    llama_context* ctx_ = nullptr;             ///< Inference context (ACTIVE)
    const llama_vocab* vocab_ = nullptr;       ///< Vocabulary (from model_)
    int last_prefill_tokens_ = 0;              ///< gh#96: prompt tokens decoded by last generate()
    int last_gen_decode_calls_ = 0;            ///< gh#98: batched-decode step count of last batch
    int last_input_tokens_ = 0;                ///< gh#97: tokenized prompt size of last generate()
    double last_prefill_ms_ = 0.0;             ///< gh#96: prefill wall-clock ms of last generate()
    std::vector<llama_token> resident_tokens_; ///< gh#96: tokens resident in KV seq 0 (warm-keep)

    /* ── v2.3.10 seam: tokenizer abstraction ─────────────── */

    /// @brief Tokenizer used by tokenize_text / do_count_tokens /
    /// internal tokenize/detokenize. Created in do_load against the
    /// loaded vocab, destroyed in do_unload BEFORE the model is
    /// freed (the vocab pointer is borrowed). Unit tests inject a
    /// MockTokenizer via the test-only constructor to exercise the
    /// LlamaCppBackend public methods without a real model.
    /// @version 2.3.10
    std::unique_ptr<Tokenizer> tokenizer_;

    /* ── v2.3.10 seam: sampler abstraction ───────────────── */

    /// @brief Factory used by the decode loop to build per-generation
    /// samplers. Created in do_activate against the live `ctx_` /
    /// `vocab_`, destroyed in do_deactivate BEFORE those resources
    /// are freed (the factory borrows them). Unit tests inject a
    /// MockSamplerFactory via inject_sampler_factory_for_test to
    /// exercise the decode-loop call site without a real context.
    /// @version 2.3.10
    std::unique_ptr<SamplerFactory> sampler_factory_;

    /* ── Prompt cache ───────────────────────────────────── */

    PromptCacheConfig prompt_cache_config_;      ///< Cache config (v1.8.3)
    std::unique_ptr<PromptCache> prompt_cache_;  ///< KV prefix cache (v1.8.3)

    /* ── gh#87 (v2.7.0): common_chat tool-call render/parse state ─ */

    std::string active_tools_json_;            ///< MCP tool defs for next render
    // LIVE capture — overwritten by EVERY render (incl. a toolless interleave
    // like the constitutional validator's critique). Serves has_common_chat_
    // params() / tool_call_close_marker() — "what THIS render produced".
    int last_chat_format_ = 0;                 ///< Captured common_chat_format
    std::string last_generation_prompt_;       ///< Captured generation_prompt
    std::string last_parser_;                  ///< Captured serialized PEG arena
    bool have_chat_params_ = false;            ///< True once a tool render captured params
    // gh#105 (v2.8.3): "sticky last-tooled" parse snapshot — written ONLY by a
    // successful render_with_tools, NEVER cleared by a toolless render. The
    // engine RE-parses the main output (engine.cpp:543) AFTER the validator's
    // toolless critique render; parse_response/common_chat_parse_reliable read
    // THIS so that interleave can't clobber the main call's parser → no more
    // zero-tool-call extraction with constitutional validation on.
    int parse_chat_format_ = 0;                ///< Last TOOLED render's format
    std::string parse_generation_prompt_;      ///< Last TOOLED render's gen prompt
    std::string parse_parser_;                 ///< Last TOOLED render's PEG arena
    bool parse_params_valid_ = false;          ///< True once a tooled render snapshotted

    /* ── Internal helpers ────────────────────────────────── */

    /**
     * @brief Tokenize text using model vocabulary.
     * @param text Input text.
     * @param add_special Add special tokens (BOS).
     * @return Token vector.
     * @version 1.8.2
     */
    std::vector<llama_token> tokenize(
        const std::string& text, bool add_special) const;

    /**
     * @brief Detokenize a single token.
     * @param token Token ID.
     * @return String representation.
     * @version 1.8.2
     */
    std::string detokenize(llama_token token) const;

    /**
     * @brief Apply chat template to messages.
     * @param messages Conversation history.
     * @param params Generation parameters (for enable_thinking).
     * @return Formatted prompt string.
     * @version 2.6.1
     */
    std::string apply_chat_template(
        const std::vector<Message>& messages,
        const GenerationParams& params) const;

    /**
     * @brief Generation render seam: common_chat-with-tools or legacy (gh#87).
     *
     * Routes to `render_with_tools` (capturing parse params) when tools have
     * been staged via `set_active_tools`, else to `apply_chat_template`
     * (clearing any stale captured params). Used by every message-generate
     * entry point so the common_chat path is reachable from generate(), not
     * just raw complete(). Raw completion + the router path bypass this.
     *
     * @param messages Conversation history.
     * @param params Generation parameters.
     * @return Formatted prompt string.
     * @internal
     * @version 2.7.0
     */
    std::string render_prompt(
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /**
     * @brief Low-level GGUF template path (gh#86 fallback, v2.6.1).
     * @param messages Conversation history.
     * @return Formatted prompt string, or a plain join on failure.
     * @version 2.6.1
     */
    std::string apply_chat_template_lowlevel(
        const std::vector<Message>& messages) const;

    /**
     * @brief Core decode loop — shared by generate and streaming.
     *
     * gh#98 (v2.8.0): the post-prefill sampling loop is now in
     * generate_after_prefill (shared with the batch path); decode_loop is
     * create_sampler + run_prefill + generate_after_prefill.
     *
     * @param tokens Input token sequence.
     * @param params Generation parameters.
     * @param on_token Per-token callback (nullptr for batch).
     * @param cancel Cancel flag (nullptr for batch).
     * @return GenerationResult.
     * @version 2.8.0
     */
    GenerationResult decode_loop(
        const std::vector<llama_token>& tokens,
        const GenerationParams& params,
        std::function<void(std::string_view)> on_token,
        std::atomic<bool>* cancel);

    /**
     * @brief The post-prefill sampling loop (extracted from decode_loop).
     *
     * gh#98 (v2.8.0): assumes the prompt is already prefilled into seq 0 (its
     * last-position logits are ready) and runs the step_token loop. Reused by
     * decode_loop (after run_prefill) and by the same-prefix batch path (after
     * decode_tokens_from of a request's suffix off the shared prefix).
     *
     * @param sampler Per-request sampler (carries its grammar).
     * @param params Generation parameters (max_tokens, stop).
     * @param on_token Streaming callback (empty for batch).
     * @param cancel Cancel flag (nullptr for batch).
     * @return GenerationResult with content/finish_reason/token_count.
     * @internal
     * @version 2.8.0
     */
    GenerationResult generate_after_prefill(
        Sampler& sampler,
        const GenerationParams& params,
        std::function<void(std::string_view)> on_token,
        std::atomic<bool>* cancel);

    /**
     * @brief Per-sequence state for the gh#98 multi-seq batched decode.
     *
     * One per request: its own grammar sampler chain (option A), its KV
     * sequence id, the batch cell holding its current logits, and its
     * accumulated output. `sampler` owns the chain; `chain` borrows it.
     * @version 2.8.0
     */
    struct BatchSeq {
        std::unique_ptr<Sampler> sampler;   ///< Owns the per-request chain
        llama_sampler* chain = nullptr;     ///< Borrowed native chain (sampled per-idx)
        llama_seq_id seq_id = 0;            ///< KV sequence id
        int pos = 0;                        ///< Next KV position to write
        int logits_idx = -1;                ///< Batch cell holding current logits
        int n_gen = 0;                      ///< Tokens generated so far
        int max_tokens = 0;                 ///< Per-request generation cap
        bool active = true;                 ///< Still generating?
        std::vector<llama_token> out;       ///< Generated tokens
        std::string finish = "stop";       ///< Finish reason
    };

    /**
     * @brief Run the gh#98 multi-seq batched decode (v2.8.0).
     *
     * Prefills the shared prefix once into seq 0, `seq_cp`s it to N sequences,
     * prefills each request's suffix, then decodes all sequences together —
     * one `llama_decode` per step over N tokens, each sampled under its own
     * grammar chain. Caller guarantees batch_is_viable (plain KV, n<=n_parallel,
     * shared>0, suffixes fit n_batch). Returns one result per request.
     * @version 2.8.0
     */
    std::vector<GenerationResult> run_batched_decode(
        const std::vector<std::vector<llama_token>>& toks,
        const std::vector<GenerationParams>& params,
        std::size_t shared,
        std::atomic<bool>& cancel);

    /// @brief Build per-request sampler chains + seq ids. @version 2.8.0
    bool prepare_batch_seqs(std::vector<BatchSeq>& seqs,
                            const std::vector<GenerationParams>& params);
    /// @brief Prefill shared prefix into seq 0 + seq_cp fan-out. @version 2.8.0
    bool prefill_shared_and_fanout(std::vector<BatchSeq>& seqs,
                                   const std::vector<llama_token>& seq0,
                                   std::size_t shared);
    /// @brief Prefill each request's suffix; set per-seq logits_idx. @version 2.8.0
    bool prefill_batch_suffixes(
        std::vector<BatchSeq>& seqs,
        const std::vector<std::vector<llama_token>>& toks,
        std::size_t shared);
    /// @brief Decode all sequences together until each finishes. @version 2.8.0
    void run_batch_gen_loop(std::vector<BatchSeq>& seqs, int max_steps,
                            std::atomic<bool>& cancel);
    /// @brief Sample+accept+classify each still-active sequence. @version 2.8.0
    void sample_batch_active(std::vector<BatchSeq>& seqs);
    /// @brief Detokenize each sequence into a GenerationResult. @version 2.8.0
    std::vector<GenerationResult> build_batch_results(
        std::vector<BatchSeq>& seqs);
    /// @brief Release every batch sequence's temp seq_id (seq 0 excluded). @version 2.8.0
    void release_temp_seqs(std::vector<BatchSeq>& seqs);

    /**
     * @brief Run batched prefill on input tokens.
     * @param tokens Input token sequence.
     * @return true on success.
     * @version 1.8.2
     */
    bool run_prefill(const std::vector<llama_token>& tokens);

    /**
     * @brief Generate one token and append to output.
     * @param sampler Sampler used for the per-token draw (v2.3.10 seam).
     * @param generated Accumulated output (mutated).
     * @param on_token Streaming callback.
     * @param stop Stop sequences.
     * @return "continue", "stop", "eos", or "error".
     * @version 2.3.10
     */
    std::string step_token(
        Sampler& sampler,
        std::string& generated,
        std::function<void(std::string_view)>& on_token,
        const std::vector<std::string>& stop);

    /**
     * @brief Build a Sampler for one generation from params.
     *
     * v2.3.10 seam: thin wrapper that delegates to
     * `sampler_factory_->create(params)`. Returns nullptr when no
     * factory has been wired (COLD backend / production never
     * activated). Callers must null-check before use.
     *
     * Kept as a member function (vs inline at every call site) so
     * the four legacy callers (`decode_loop`, `run_sampling_loop`,
     * `do_generate_text_only`, `do_generate_streaming_text_only`)
     * stay a one-liner — minimum diff to ship the seam.
     *
     * @param params Generation parameters.
     * @return Owned Sampler, or nullptr if no factory wired.
     * @version 2.3.10
     */
    std::unique_ptr<Sampler> create_sampler(
        const GenerationParams& params) const;

    /**
     * @brief Extract the system prompt from messages.
     * @param messages Conversation history.
     * @return System prompt text, empty if no system message.
     * @version 1.8.3
     */
    static std::string extract_system_prompt(
        const std::vector<Message>& messages);

    /**
     * @brief Run prefill with prompt cache integration.
     * @param tokens Full token sequence.
     * @param system_prompt System prompt text for cache key.
     * @param messages Original messages (for prefix boundary).
     * @param params Generation parameters.
     * @return true on success.
     * @version 1.8.3
     */
    bool run_prefill_cached(
        const std::vector<llama_token>& tokens,
        const std::string& system_prompt,
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /**
     * @brief Cache-aware prefill dispatch (gh#96 v2.7.5: extracted body of
     *        run_prefill_cached so the wrapper owns the perf reset+capture).
     * @param tokens Full token sequence.
     * @param system_prompt System prompt text for cache key.
     * @param messages Original messages (for prefix boundary).
     * @param params Generation parameters.
     * @return true on success.
     * @version 2.7.5
     */
    bool prefill_dispatch(
        const std::vector<llama_token>& tokens,
        const std::string& system_prompt,
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /**
     * @brief gh#96 (v2.7.5): try incremental prefill against resident KV.
     *
     * If warm-keep is enabled and the resident KV holds a usable token-prefix
     * of `tokens` (warm_keep_cut > 0), seq_rm the divergent tail and decode
     * only the delta, advancing in place. Returns false (no KV mutation) to
     * fall back to a cold prefill on: warm-keep off, no/short resident match,
     * or out-of-band KV mutation (occupancy mismatch).
     * @param tokens Full incoming token sequence.
     * @return true if reuse handled the prefill; false to fall back.
     * @version 2.7.5
     */
    bool try_warm_reuse(const std::vector<llama_token>& tokens);

    /**
     * @brief gh#96 (v2.7.5): drop the warm-keep resident-KV record.
     *
     * Called by every non-text-cached generate path (multimodal, complete,
     * speculative) that mutates seq 0 out-of-band, so the next text turn
     * cannot reuse a stale record.
     * @utility
     * @internal
     * @version 2.7.5
     */
    void invalidate_resident_kv();

    /**
     * @brief Decode tokens starting at a given offset.
     * @param tokens Full token sequence.
     * @param start_offset First token to decode.
     * @return true on success.
     * @version 2.0.6
     */
    bool decode_tokens_from(
        const std::vector<llama_token>& tokens, int start_offset);

    /**
     * @brief Restore KV state from cache and decode remaining tokens.
     * @param cached Cache entry to restore.
     * @param tokens Full token sequence.
     * @return true on success, false to fall back to full prefill.
     * @version 2.0.6
     */
    bool restore_cached_prefix(
        const CacheEntry* cached,
        const std::vector<llama_token>& tokens);

    /**
     * @brief Two-pass prefill: prefix-only prefill → save → rest.
     * @param tokens Full token sequence.
     * @param prefix_tokens System prefix token count.
     * @param key Cache key to store under.
     * @return true on success.
     * @version 2.0.6
     */
    bool prefill_and_cache_prefix(
        const std::vector<llama_token>& tokens,
        int prefix_tokens,
        const CacheKey& key);

    /**
     * @brief Capture seq 0 KV state and store under the given key.
     * @param key Cache key.
     * @param prefix_tokens Token count to record with the entry.
     * @version 2.0.6
     */
    void save_prefix_to_cache(const CacheKey& key, int prefix_tokens);

    /**
     * @brief Compute token count of system messages only.
     * @param messages Message list.
     * @param params Generation params (for template).
     * @return Token count, 0 if no system messages.
     * @version 1.8.3
     */
    int compute_prefix_token_count(
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /* ── Evaluation helpers (v1.9.10) ───────────────────── */

    /**
     * @brief Allocate a temporary sequence ID for evaluation.
     * @return Unused seq_id, or -1 if pool is exhausted.
     * @version 1.9.10
     */
    llama_seq_id allocate_temp_seq_id();

    /**
     * @brief Release a temporary sequence ID back to the pool.
     * @param seq_id The seq_id to release.
     * @version 1.9.10
     */
    void release_temp_seq_id(llama_seq_id seq_id);

    /**
     * @brief Extract log-probability for a token from logits.
     *
     * Computes log_softmax(logits)[next_token] using the numerically
     * stable form: logits[t] - max - log(sum(exp(logits - max))).
     *
     * @param logits Raw logits array from llama_get_logits_ith().
     * @param next_token The token to score.
     * @param n_vocab Vocabulary size.
     * @return log P(next_token | context).
     * @version 1.9.10
     */
    static float extract_token_logprob(
        const float* logits,
        int32_t next_token,
        int n_vocab);

    std::mutex seq_id_mutex_;                 ///< Guards temp seq_id pool (v1.9.10)
    std::vector<llama_seq_id> free_seq_ids_;  ///< Available temporary seq_ids (v1.9.10)
    /// gh#98: monotonic high-water for NEW temp seq_ids (the old `1 + size()`
    /// handed out duplicates when the pool was empty — every call returned 1 —
    /// colliding the batch's sequences). Released ids return to free_seq_ids_
    /// and are reused first, so this stays bounded by concurrent demand.
    llama_seq_id next_temp_seq_id_ = 1;

    /* ── Architecture detection (v1.9.13) ──────────────── */

    /// @brief True if loaded model is recurrent (GDN/Mamba/RWKV).
    /// Set during do_load() from llama_model_is_recurrent(). Drives
    /// capability reporting (KV_CACHE vs HIDDEN_STATE, speculative
    /// decoding compatibility, etc.).
    /// @version 1.9.13
    bool is_recurrent_ = false;
    bool is_hybrid_ = false;                   ///< gh#97: attention + recurrent/SSM memory

    /**
     * @brief Check if loaded model is recurrent.
     * @return true if GDN/Mamba/RWKV architecture.
     * @version 1.9.13
     */
    bool is_recurrent() const;

    /* ── Vision / multimodal (v1.9.11 Phases 5–7 + v2.1.8) ── */

    /// @brief libmtmd context, or nullptr if no mmproj loaded.
    /// Allocated in do_activate() when ModelConfig::mmproj_path is
    /// set; freed in do_deactivate()/do_unload(). The leading `::`
    /// is required — without it `mtmd_context` resolves into the
    /// `entropic::` namespace and the forward declaration becomes
    /// an incompatible incomplete type at the call sites.
    /// @version 2.1.8
    ::mtmd_context* mtmd_ctx_ = nullptr;

    /// @brief Cached `mtmd_support_vision(mtmd_ctx_)` result.
    /// @version 2.1.8
    bool has_vision_ = false;

    /**
     * @brief Multimodal generation core (v1.9.11 Phases 5–7).
     *
     * Runs the libmtmd-backed prefill + decode for messages whose
     * `content_parts` contain image entries. Builds an mtmd_bitmap
     * list from ContentPart paths, inserts media markers in the
     * chat-formatted prompt, then calls mtmd_helper_eval_chunks to
     * encode images and decode all chunks in order.
     *
     * After eval, sampling proceeds via the normal step_token loop
     * — the cache state is positioned past the multimodal prefill.
     *
     * @param messages Conversation history (must contain images).
     * @param params Generation parameters.
     * @param on_token Per-token callback (nullptr for batch mode).
     * @param cancel Cancel atomic (nullptr for batch mode).
     * @return GenerationResult.
     * @version 2.1.8
     */
    GenerationResult generate_multimodal(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>* cancel);

    /**
     * @brief Initialize the libmtmd context if mmproj is configured.
     * @utility
     * @version 2.1.8
     */
    void init_mmproj_if_configured();

    /**
     * @brief Load the GGUF model onto the GPU (do_activate step 1).
     *
     * Extracted from do_activate to keep it knots-clean. Sets model_ +
     * vocab_ on success; sets last_error_ and returns false otherwise.
     *
     * @return true on success.
     * @internal
     * @version 2.3.7
     */
    bool load_gpu_model();

    /**
     * @brief Create the llama context + prompt cache (do_activate step 2).
     *
     * Extracted from do_activate. Builds ctx_ from model_ and the
     * configured context params, then lazily creates the prompt cache.
     *
     * @return true on success; sets last_error_ on failure.
     * @internal
     * @version 2.3.7
     */
    bool create_inference_context();

    /**
     * @brief Run mtmd_tokenize + mtmd_helper_eval_chunks on a prompt.
     * @param prompt Marker-substituted chat-formatted prompt.
     * @param bitmaps Loaded image bitmaps in marker order (borrowed).
     * @param[out] err_msg Filled on failure.
     * @return ENTROPIC_OK on success, error code on failure.
     * @utility
     * @version 2.1.8
     */
    entropic_error_t mtmd_prefill(
        const std::string& prompt,
        const std::vector<::mtmd_bitmap*>& bitmaps,
        std::string& err_msg);

    /**
     * @brief Sample tokens until stop / max_tokens / cancel.
     *
     * Shared by generate_multimodal and the text-only paths after
     * prefill. Operates on the already-positioned `ctx_` KV cache.
     *
     * @param params Generation parameters.
     * @param on_token Per-token callback (nullable).
     * @param cancel Atomic cancel flag (nullable).
     * @param t0 Generation start time for finalize_result.
     * @return GenerationResult.
     * @utility
     * @version 2.1.8
     */
    GenerationResult run_sampling_loop(
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>* cancel,
        const std::chrono::steady_clock::time_point& t0);

    /**
     * @brief Text-only batch generation (extracted from do_generate).
     * @utility
     * @version 2.1.8
     */
    GenerationResult do_generate_text_only(
        const std::vector<Message>& messages,
        const GenerationParams& params);

    /**
     * @brief Text-only batch generation with per-token cancel poll.
     *
     * gh#81 (v2.4.2): mirrors the per-token cancel poll already in
     * `do_generate_streaming_text_only`. When `cancel` is set
     * mid-decode the loop breaks within one token; the result is
     * tagged `finish_reason="cancelled"` and
     * `error_code=ENTROPIC_ERROR_CANCELLED`.
     *
     * @utility
     * @version 2.4.2
     */
    GenerationResult do_generate_text_only(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::atomic<bool>& cancel);

    /**
     * @brief Text-only streaming generation (extracted from streaming).
     * @utility
     * @version 2.1.8
     */
    GenerationResult do_generate_streaming_text_only(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel);
};

} // namespace entropic
