// SPDX-License-Identifier: Apache-2.0
/**
 * @file llama_cpp_sampler.h
 * @brief Concrete llama.cpp Sampler + SamplerFactory (v2.3.10 seam impl).
 *
 * Implements the abstract Sampler / SamplerFactory interfaces against
 * llama.cpp's `llama_sampler` chain primitive. Construction transfers
 * the chain-building logic verbatim from the legacy
 * `LlamaCppBackend::create_sampler` — chain order, gates, and the
 * v2.3.10 min_p block (gh#23) are preserved bit-for-bit so production
 * output is unchanged.
 *
 * Lifetime contract:
 *   - The factory borrows `llama_context*` and `const llama_vocab*`
 *     pointers; `LlamaCppBackend::do_deactivate` must destroy the
 *     factory before freeing those resources.
 *   - Each Sampler returned by `create()` owns its `llama_sampler*`
 *     and frees it in the destructor.
 *
 * @internal
 * @version 2.3.10
 */

#pragma once

#include <entropic/inference/sampler.h>

// Forward-declare llama.cpp opaque types — keeps llama.h out of the
// header. The .cpp pulls in llama.h for real calls.
struct llama_context;
struct llama_vocab;
struct llama_sampler;

namespace entropic {

/**
 * @brief Sampler adapter that wraps a `llama_sampler*` chain.
 *
 * Owns the chain (freed in destructor). Stores a non-owning
 * `llama_context*` snapshot captured at factory creation time;
 * the backend guarantees the context outlives every Sampler it
 * vends.
 *
 * @internal
 * @version 2.3.10
 */
class LlamaCppSampler : public Sampler {
public:
    /**
     * @brief Construct with an already-built llama_sampler chain.
     * @param chain Owning pointer to llama_sampler chain.
     * @param ctx   Non-owning llama_context (snapshot at sampler birth).
     * @version 2.3.10
     */
    LlamaCppSampler(llama_sampler* chain, llama_context* ctx);

    ~LlamaCppSampler() override;

    LlamaCppSampler(const LlamaCppSampler&) = delete;
    LlamaCppSampler& operator=(const LlamaCppSampler&) = delete;

    int32_t sample() override;

    /**
     * @brief Reset llama_sampler internal state.
     *
     * Forwards to `llama_sampler_reset` so callers re-seeding a
     * generation pick up a fresh RNG / penalty-window state.
     *
     * @version 2.3.10
     */
    void reset() override;

    /**
     * @brief Expose the underlying chain for legacy call sites
     *        that have not yet been ported to the Sampler API.
     *
     * Production callers in the decode loop should use `sample()`
     * directly; this accessor exists for transition-period code
     * and for tests asserting that production wiring really
     * produces a non-null chain.
     *
     * @return Borrowed `llama_sampler*` (never null for the
     *         production Sampler; do not free).
     * @version 2.3.10
     */
    llama_sampler* native_chain() const { return chain_; }

private:
    llama_sampler* chain_ = nullptr;  ///< Owned chain (freed in dtor)
    llama_context* ctx_   = nullptr;  ///< Borrowed context
};

/**
 * @brief Factory that builds entropic's canonical llama_sampler chain.
 *
 * Chain order (must remain stable for output reproducibility):
 *   grammar → penalties → temperature → top-k → top-p → min-p → dist
 *
 * Gating semantics:
 *   - grammar:    appended only when `params.grammar` is non-empty
 *                 (and llama_sampler_init_grammar returns non-null).
 *   - penalties:  appended when `params.repeat_penalty != 1.0f`.
 *   - temperature: appended when `params.temperature > 0.0f`
 *                 (temperature == 0 ⇒ greedy mode; the chain skips
 *                 the temp sampler entirely).
 *   - top-k:      appended when `params.top_k > 0`.
 *   - top-p:      appended when `params.top_p < 1.0f`.
 *   - min-p (v2.3.10, gh#23): appended when `params.min_p > 0.0f`.
 *                 Default 0.0f preserves pre-v2.3.10 chain shape.
 *   - dist:       always appended last; seed resolves via
 *                 `LLAMA_DEFAULT_SEED` when `params.seed < 0`. (P2-14)
 *
 * @internal
 * @version 2.3.10
 */
class LlamaCppSamplerFactory : public SamplerFactory {
public:
    /**
     * @brief Construct with borrowed context + vocab pointers.
     *
     * @param ctx   Active llama_context (must outlive this factory).
     * @param vocab Vocab pointer (must outlive this factory).
     * @version 2.3.10
     */
    LlamaCppSamplerFactory(llama_context* ctx, const llama_vocab* vocab);

    std::unique_ptr<Sampler> create(
        const GenerationParams& params) override;

private:
    llama_context* ctx_      = nullptr;  ///< Borrowed context
    const llama_vocab* vocab_ = nullptr; ///< Borrowed vocab
};

} // namespace entropic
