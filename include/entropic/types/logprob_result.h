/**
 * @file logprob_result.h
 * @brief Per-token log-probability evaluation result.
 *
 * Returned by InferenceBackend::evaluate_logprobs(). Contains per-token
 * log-probabilities, the input tokens echoed back for verification, and
 * aggregate metrics (perplexity, total log-probability).
 *
 * @par Indexing
 * For a sequence of N input tokens, logprobs contains N-1 values:
 *   logprobs[i] = log P(tokens[i+1] | tokens[0..i])
 *
 * @par Perplexity
 *   perplexity = exp(-1/(N-1) * sum(logprobs))
 *
 * @version 1.9.10
 */

#pragma once

#include <cstdint>
#include <vector>

namespace entropic {

/**
 * @brief Per-token log-probability evaluation result.
 *
 * Produced by InferenceBackend::evaluate_logprobs(). The base class
 * computes perplexity and total_logprob from the raw logprobs returned
 * by the backend implementation.
 *
 * @version 1.9.10
 */
struct LogprobResult {
    std::vector<float> logprobs;       ///< Log-prob for each token transition (N-1 values)
    std::vector<int32_t> tokens;       ///< Input tokens echoed back for verification
    float perplexity = 0.0f;           ///< exp(-mean(logprobs)) — lower = less surprising
    float total_logprob = 0.0f;        ///< Sum of all logprob values
    int n_tokens = 0;                  ///< Number of input tokens
    int n_logprobs = 0;                ///< Number of logprob values (n_tokens - 1)
};

} // namespace entropic
