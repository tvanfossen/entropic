// SPDX-License-Identifier: Apache-2.0
/**
 * @file llama_cpp_sampler.cpp
 * @brief LlamaCppSampler + LlamaCppSamplerFactory implementation
 *        (v2.3.10 seam impl).
 *
 * Chain construction logic was lifted verbatim from the legacy
 * `LlamaCppBackend::create_sampler`. The gates (`temperature > 0`,
 * `repeat_penalty != 1`, etc.) and ordering
 * (grammar → penalties → temperature → top-k → top-p → min-p → dist)
 * are unchanged. This file is uncovered by the CPU-only unit-test
 * gate by design — its job is to be a thin pass-through to llama.cpp.
 * Coverage of the LOGIC that consumes Samplers lives in
 * LlamaCppBackend tests (which mock the SamplerFactory).
 *
 * @internal
 * @internal
 * @version 2.3.10
 */

#include "llama_cpp_sampler.h"
#include <entropic/types/logging.h>

#include <llama.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

static auto logger = entropic::log::get("inference.sampler");

namespace entropic {

namespace {

/**
 * @brief Append grammar sampler to chain when a grammar string is set.
 *
 * Lifted from `llama_cpp_backend.cpp` (v2.3.10) so the gh#23 chain
 * construction stays exactly where the original logic lived. v2.7.4
 * adds gh#95 observability: an info log on successful attach and an
 * error log if `llama_sampler_init_grammar` returns null (output would
 * then be silently unconstrained).
 *
 * @utility
 * @internal
 * @version 2.7.4
 */
void add_grammar_sampler(llama_sampler* chain,
                         const llama_vocab* vocab,
                         const std::string& grammar) {
    if (grammar.empty()) { return; }
    llama_sampler* g = llama_sampler_init_grammar(
        vocab, grammar.c_str(), "root");
    if (g) {
        llama_sampler_chain_add(chain, g);
        // gh#95: surface grammar attachment — the issue (and the
        // tool-staged enforcement gap) had no log to confirm the sampler
        // actually engaged the constraint.
        logger->info("Grammar sampler attached ({} bytes)", grammar.size());
    } else {
        logger->error("Grammar sampler init FAILED for root rule — output "
                      "will be UNCONSTRAINED. Grammar ({} bytes): {}",
                      grammar.size(), grammar);
    }
}

/**
 * @brief Add the gh#23 v2.3.16 `logit_bias` stage when the map is non-empty.
 *
 * Builds a `std::vector<llama_logit_bias>` from the map and calls
 * `llama_sampler_init_logit_bias`. No-op on empty map so the
 * pre-v2.3.16 chain shape stays bit-for-bit identical.
 * @utility
 * @internal
 * @version 2.3.16
 */
void add_logit_bias_sampler(llama_sampler* chain,
                            const llama_vocab* vocab,
                            const std::unordered_map<int32_t, float>& biases) {
    if (biases.empty()) { return; }
    std::vector<llama_logit_bias> entries;
    entries.reserve(biases.size());
    for (auto& [tok, val] : biases) {
        entries.push_back({tok, val});
    }
    llama_sampler_chain_add(chain,
        llama_sampler_init_logit_bias(
            llama_vocab_n_tokens(vocab),
            static_cast<int32_t>(entries.size()),
            entries.data()));
}

/**
 * @brief Resolve caller-supplied seed to a llama-compatible uint32.
 *
 * P2-14: negative seed maps to LLAMA_DEFAULT_SEED (random). Lifted
 * v2.3.10 from `llama_cpp_backend.cpp` together with chain building.
 *
 * @utility
 * @internal
 * @version 2.3.10
 */
uint32_t resolve_dist_seed(int caller_seed) {
    return caller_seed < 0
        ? LLAMA_DEFAULT_SEED
        : static_cast<uint32_t>(caller_seed);
}

} // anonymous namespace

// ── LlamaCppSampler ────────────────────────────────────────

/**
 * @brief Construct an LlamaCppSampler wrapping a pre-built sampler chain.
 * @internal
 * @version 2.3.10
 */
LlamaCppSampler::LlamaCppSampler(llama_sampler* chain, llama_context* ctx)
    : chain_(chain), ctx_(ctx) {}

/**
 * @brief Free the underlying llama.cpp sampler chain.
 * @internal
 * @version 2.3.10
 */
LlamaCppSampler::~LlamaCppSampler() {
    if (chain_) {
        llama_sampler_free(chain_);
        chain_ = nullptr;
    }
}

/**
 * @brief Sample one token from the current logits via the wrapped chain.
 * @return Sampled token id, or -1 if the chain or context is missing.
 * @internal
 * @version 2.3.10
 */
int32_t LlamaCppSampler::sample() {
    if (chain_ == nullptr || ctx_ == nullptr) { return -1; }
    return llama_sampler_sample(chain_, ctx_, -1);
}

/**
 * @brief Reset stateful samplers (e.g. repeat-penalty history) on the chain.
 * @internal
 * @version 2.3.10
 */
void LlamaCppSampler::reset() {
    if (chain_ != nullptr) {
        llama_sampler_reset(chain_);
    }
}

// ── LlamaCppSamplerFactory ─────────────────────────────────

/**
 * @brief Construct a factory bound to a llama_context + vocab.
 * @internal
 * @version 2.3.10
 */
LlamaCppSamplerFactory::LlamaCppSamplerFactory(
    llama_context* ctx, const llama_vocab* vocab)
    : ctx_(ctx), vocab_(vocab) {}

/**
 * @brief Build the v2.3.10 sampler chain from GenerationParams.
 *
 * Chain order: grammar → penalties → temperature → top-k → top-p →
 * min-p → dist. Each stage is gated by its parameter so the default
 * chain stays bit-identical to pre-v2.3.10.
 * @param params Generation parameters driving each stage's gate.
 * @return Owned Sampler that wraps the constructed llama.cpp chain.
 * @internal
 * @version 2.3.10
 */
std::unique_ptr<Sampler> LlamaCppSamplerFactory::create(
    const GenerationParams& params)
{
    llama_sampler_chain_params chain_params =
        llama_sampler_chain_default_params();
    llama_sampler* chain = llama_sampler_chain_init(chain_params);

    add_grammar_sampler(chain, vocab_, params.grammar);
    add_logit_bias_sampler(chain, vocab_, params.logit_bias);

    // gh#23 MVP items 2 + 3 (v2.3.14 + v2.3.15): the penalties sampler
    // now also carries presence_penalty (4th arg) and frequency_penalty
    // (3rd arg). Gate fires when ANY of repeat / presence / frequency
    // is non-default, so any single knob is sufficient to activate
    // the stage.
    if (params.repeat_penalty != 1.0f
        || params.presence_penalty > 0.0f
        || params.frequency_penalty > 0.0f) {
        llama_sampler_chain_add(chain,
            llama_sampler_init_penalties(
                64, params.repeat_penalty,
                params.frequency_penalty,
                params.presence_penalty));
    }
    if (params.temperature > 0.0f) {
        llama_sampler_chain_add(chain,
            llama_sampler_init_temp(params.temperature));
    }
    if (params.top_k > 0) {
        llama_sampler_chain_add(chain,
            llama_sampler_init_top_k(params.top_k));
    }
    if (params.top_p < 1.0f) {
        llama_sampler_chain_add(chain,
            llama_sampler_init_top_p(params.top_p, 1));
    }
    // Min-P (gh#23 MVP item 1, v2.3.10) — gated so default 0.0f is no-op
    if (params.min_p > 0.0f) {
        llama_sampler_chain_add(chain,
            llama_sampler_init_min_p(params.min_p, 1));
    }

    llama_sampler_chain_add(chain,
        llama_sampler_init_dist(resolve_dist_seed(params.seed)));

    return std::make_unique<LlamaCppSampler>(chain, ctx_);
}

} // namespace entropic
