// SPDX-License-Identifier: Apache-2.0
/**
 * @file warm_keep_util.h
 * @brief gh#96 (v2.7.5) warm-keep / incremental-prefill decision logic.
 *
 * Pure functions (no llama.cpp, no I/O) so the correctness-critical reuse
 * decision is unit-testable on the CPU pre-commit tier. The backend wraps
 * these with the actual KV mutation (seq_rm + delta decode).
 *
 * @version 2.7.5
 */
#ifndef ENTROPIC_INFERENCE_WARM_KEEP_UTIL_H
#define ENTROPIC_INFERENCE_WARM_KEEP_UTIL_H

#include <algorithm>
#include <cstddef>
#include <vector>

namespace entropic {

/**
 * @brief Length of the longest common token prefix of two sequences.
 * @param a First sequence (resident KV tokens).
 * @param b Second sequence (incoming prompt tokens).
 * @return Number of leading tokens that match position-for-position.
 * @utility
 * @version 2.7.5
 */
template <typename Tok>
inline std::size_t common_prefix_len(const std::vector<Tok>& a,
                                     const std::vector<Tok>& b) {
    std::size_t n = std::min(a.size(), b.size());
    std::size_t i = 0;
    while (i < n && a[i] == b[i]) {
        ++i;
    }
    return i;
}

/**
 * @brief Decide how many resident-KV tokens warm-keep may reuse this turn.
 *
 * Returns the cut: tokens [0, cut) are kept in KV (the divergent tail is
 * seq_rm'd) and the incoming sequence is decoded from `cut` onward. A return
 * of 0 means fall back to a cold prefill. Rules:
 *  - no resident KV, or an incoming prompt shorter than 2 tokens → 0;
 *  - the KV must actually hold the matched prefix (kv_pos_max+1 >= common),
 *    else something mutated seq 0 out-of-band (multimodal / complete /
 *    speculative / another conversation) → 0, fall back;
 *  - never reuse the whole sequence — always re-decode at least the final
 *    token so its logits are fresh for sampling (cut < incoming.size()).
 *
 * @param resident Tokens recorded as resident in KV seq 0 (the last prompt).
 * @param incoming The new prompt's tokens.
 * @param kv_pos_max Highest occupied position in seq 0 (llama_memory_seq_pos_max),
 *                   or < 0 if empty.
 * @return Reuse cut in [0, incoming.size()); 0 = cold fallback.
 * @utility
 * @version 2.7.5
 */
template <typename Tok>
inline std::size_t warm_keep_cut(const std::vector<Tok>& resident,
                                 const std::vector<Tok>& incoming,
                                 long kv_pos_max) {
    if (resident.empty() || incoming.size() < 2 || kv_pos_max < 0) {
        return 0;
    }
    std::size_t common = common_prefix_len(resident, incoming);
    if (common == 0) {
        return 0;
    }
    if (static_cast<std::size_t>(kv_pos_max) + 1 < common) {
        return 0;  // KV does not actually hold the matched prefix
    }
    if (common >= incoming.size()) {
        common = incoming.size() - 1;  // always re-decode the last token
    }
    return common;
}

}  // namespace entropic

#endif  // ENTROPIC_INFERENCE_WARM_KEEP_UTIL_H
