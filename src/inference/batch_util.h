// SPDX-License-Identifier: Apache-2.0
/**
 * @file batch_util.h
 * @brief gh#98 (v2.8.0) same-prefix batch-generation decision logic.
 *
 * Pure functions (no llama.cpp, no I/O) so the correctness-critical shared-
 * prefix computation is unit-testable on the CPU pre-commit tier. The backend
 * wraps these with the actual KV mutation (prefill seq 0 → seq_cp fan-out →
 * per-seq suffix decode → batched generation).
 *
 * @version 2.8.0
 */
#ifndef ENTROPIC_INFERENCE_BATCH_UTIL_H
#define ENTROPIC_INFERENCE_BATCH_UTIL_H

#include <algorithm>
#include <cstddef>
#include <vector>

#include "warm_keep_util.h"  // common_prefix_len

namespace entropic {

/**
 * @brief Longest shared token prefix across N request sequences (gh#98).
 *
 * The number of leading tokens identical across ALL sequences — prefilled
 * once into seq 0 and `seq_cp`'d to the other N-1 sequences before each
 * request's unique suffix is decoded. Mirrors warm-keep's rule that at least
 * the final token of every sequence must be re-decoded so its logits are
 * fresh for sampling: the result is capped at `shortest_len - 1`, so every
 * sequence retains a non-empty suffix.
 *
 * Returns 0 (no sharing worthwhile) when fewer than 2 sequences are given,
 * any sequence is empty, or the shortest sequence is a single token.
 *
 * @param seqs Tokenized request prompts.
 * @return Shared prefix length in [0, shortest_len - 1].
 * @utility
 * @version 2.8.0
 */
template <typename Tok>
inline std::size_t batch_shared_prefix_len(
    const std::vector<std::vector<Tok>>& seqs) {
    if (seqs.size() < 2) { return 0; }
    std::size_t shortest = seqs[0].size();
    std::size_t shared = seqs[0].size();
    for (std::size_t i = 1; i < seqs.size(); ++i) {
        shortest = std::min(shortest, seqs[i].size());
        shared = std::min(shared, common_prefix_len(seqs[0], seqs[i]));
    }
    if (shortest == 0) { return 0; }
    return std::min(shared, shortest - 1);
}

/**
 * @brief Decide whether the same-prefix batch fast-path is safe + worthwhile.
 *
 * When false, the backend falls back to running each request serially (the
 * proven single-request path) — which is always correct, just without the
 * shared-prefill win. The fast-path is taken only when ALL hold:
 *  - at least 2 requests (nothing to share with one);
 *  - the arch is NOT hybrid/recurrent (seq-level KV ops are unsafe there —
 *    gh#97; recurrent memory rejects partial seq_rm/seq_cp);
 *  - the batch fits the context's sequence slots (n <= n_parallel);
 *  - there is a real shared prefix to amortize (shared > 0);
 *  - the suffix tokens fit one decode batch (total_suffix <= n_batch), so the
 *    fan-out prefill never overruns the batch capacity.
 *
 * @param n Number of requests.
 * @param n_parallel Context sequence-slot capacity (config n_parallel).
 * @param shared Shared prefix length (batch_shared_prefix_len result).
 * @param hybrid True if the model is hybrid/recurrent.
 * @param total_suffix Sum over requests of (len_i - shared).
 * @param n_batch Decode batch capacity (config n_batch).
 * @return true if the batch fast-path may be used.
 * @utility
 * @version 2.8.0
 */
inline bool batch_is_viable(std::size_t n, int n_parallel, std::size_t shared,
                            bool hybrid, std::size_t total_suffix,
                            int n_batch) {
    return n >= 2
        && !hybrid
        && n_parallel >= 1
        && n <= static_cast<std::size_t>(n_parallel)
        && shared > 0
        && total_suffix <= static_cast<std::size_t>(n_batch);
}

}  // namespace entropic

#endif  // ENTROPIC_INFERENCE_BATCH_UTIL_H
