// SPDX-License-Identifier: Apache-2.0
/**
 * @file sampler.h
 * @brief Abstract Sampler seam for backend testability (v2.3.10).
 *
 * Pre-v2.3.10 `LlamaCppBackend` built its `llama_sampler_chain`
 * inline from `GenerationParams` and invoked `llama_sampler_sample`
 * directly in the decode loop. Every line of chain construction
 * was therefore unreachable under the CPU-only unit-test coverage
 * gate (no real model, no real context). This abstract seam
 * relocates chain construction behind a `SamplerFactory` and
 * relocates per-token sampling behind a `Sampler` — both
 * mockable. The production backend wires `LlamaCppSamplerFactory`
 * (which builds the real llama.cpp chain); unit tests inject a
 * mock factory + mock sampler and assert behavior without a
 * loaded model.
 *
 * Pairs structurally with the v2.3.10 Tokenizer seam (see
 * `tokenizer.h`).
 *
 * @version 2.3.10
 */

#pragma once

#include <entropic/types/config.h>

#include <cstdint>
#include <memory>

namespace entropic {

/**
 * @brief Pure-virtual per-generation sampler used by the decode loop.
 *
 * One Sampler instance represents one configured sampling chain
 * — its lifetime is bounded by a single generate / streaming /
 * speculative call. Implementations capture whatever backend
 * state they need (e.g. the `llama_context*`) at construction
 * and free their internal resources in their destructor; callers
 * neither know nor care what's underneath.
 *
 * Implementations must never throw and must never crash on
 * repeated `sample()` invocations beyond model EOS.
 *
 * @version 2.3.10
 */
class Sampler {
public:
    virtual ~Sampler() = default;

    /**
     * @brief Sample one token from the current decode position.
     *
     * The backend has already positioned its KV cache to the
     * token-to-be-sampled prior to calling `sample()`. The
     * Sampler is responsible only for converting the latest
     * logits into a token id.
     *
     * @return Chosen token id, or a negative value on internal
     *         error (callers treat <0 as a hard failure).
     * @version 2.3.10
     */
    virtual int32_t sample() = 0;

    /**
     * @brief Reset internal sampler state.
     *
     * Default no-op. Mock implementations may override to clear
     * accumulated history (e.g. for tests that reuse a sampler
     * across logical generations). Production llama.cpp samplers
     * are created fresh per generation, so this is largely a
     * test-facing hook.
     *
     * @version 2.3.10
     */
    virtual void reset() {}
};

/**
 * @brief Factory that materializes a Sampler from GenerationParams.
 *
 * Held by `LlamaCppBackend` for the duration of its ACTIVE state.
 * The decode loop calls `create()` once per generation. The
 * concrete production impl (`LlamaCppSamplerFactory`) snapshots
 * the active `llama_context*` and `llama_vocab*` at construction;
 * the backend's `do_deactivate` destroys the factory BEFORE
 * freeing those resources so no dangling-pointer window exists.
 *
 * @version 2.3.10
 */
class SamplerFactory {
public:
    virtual ~SamplerFactory() = default;

    /**
     * @brief Build a configured Sampler for one generation.
     *
     * Ownership transfers to the caller. Implementations must
     * honor the entropic chain ordering / gating contract
     * (see `LlamaCppSamplerFactory::create` for the canonical
     * production rules — grammar → penalties → temperature →
     * top-k → top-p → min-p → dist).
     *
     * @param params Generation parameters (sampler knobs + seed).
     * @return Owned Sampler ready for use in the decode loop.
     * @version 2.3.10
     */
    virtual std::unique_ptr<Sampler> create(
        const GenerationParams& params) = 0;
};

} // namespace entropic
