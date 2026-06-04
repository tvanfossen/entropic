// SPDX-License-Identifier: Apache-2.0
/**
 * @file warm_keep_util_test.cpp
 * @brief gh#96 (v2.7.5): unit tests for the warm-keep reuse decision logic.
 *
 * These are the correctness-critical pure functions behind incremental
 * prefill — the longest-common-token-prefix scan and the reuse-cut decision
 * (including the occupancy gate that catches out-of-band KV mutation). Pure,
 * deterministic, CPU — runs every commit, so the silent-corruption guards
 * are validated without a GPU.
 *
 * @version 2.7.5
 */

#include <catch2/catch_test_macros.hpp>

#include "../../../src/inference/warm_keep_util.h"

#include <vector>

using entropic::common_prefix_len;
using entropic::warm_keep_cut;
using Toks = std::vector<int>;

TEST_CASE("common_prefix_len matches leading tokens", "[gh96][warm_keep]") {
    CHECK(common_prefix_len(Toks{}, Toks{}) == 0u);
    CHECK(common_prefix_len(Toks{1, 2, 3}, Toks{}) == 0u);
    CHECK(common_prefix_len(Toks{1, 2, 3}, Toks{9, 9}) == 0u);
    CHECK(common_prefix_len(Toks{1, 2, 3}, Toks{1, 2, 9}) == 2u);
    CHECK(common_prefix_len(Toks{1, 2, 3}, Toks{1, 2, 3}) == 3u);
    // One is a strict prefix of the other.
    CHECK(common_prefix_len(Toks{1, 2}, Toks{1, 2, 3, 4}) == 2u);
    CHECK(common_prefix_len(Toks{1, 2, 3, 4}, Toks{1, 2}) == 2u);
}

TEST_CASE("warm_keep_cut: happy path reuses the shared prefix",
          "[gh96][warm_keep]") {
    // Resident = prior prompt; incoming = prior prompt + appended delta.
    Toks resident{10, 11, 12, 13};          // 4 tokens resident in KV
    Toks incoming{10, 11, 12, 13, 20, 21};  // same prefix + 2 new
    // KV holds positions 0..3 (pos_max=3); plus generated tokens beyond are
    // fine — pos_max only needs to cover the matched prefix.
    CHECK(warm_keep_cut(resident, incoming, /*pos_max=*/3) == 4u);
    // Generated tokens left KV at a higher pos_max — still reuse the prefix.
    CHECK(warm_keep_cut(resident, incoming, /*pos_max=*/11) == 4u);
}

TEST_CASE("warm_keep_cut: divergence mid-history caps reuse at the cut",
          "[gh96][warm_keep]") {
    // Models a mid-history prune/compaction: token 2 was rewritten.
    Toks resident{10, 11, 12, 13, 14};
    Toks incoming{10, 11, 99, 30, 31};  // diverges at index 2
    CHECK(warm_keep_cut(resident, incoming, /*pos_max=*/4) == 2u);
}

TEST_CASE("warm_keep_cut: no shared prefix falls back (tier/system swap)",
          "[gh96][warm_keep]") {
    Toks resident{10, 11, 12};
    Toks incoming{77, 78, 79};  // different system prompt → diverges at 0
    CHECK(warm_keep_cut(resident, incoming, /*pos_max=*/2) == 0u);
}

TEST_CASE("warm_keep_cut: occupancy gate catches out-of-band KV wipe",
          "[gh96][warm_keep]") {
    // resident_tokens_ still records 4 tokens, but a multimodal/complete/spec
    // path (or another conversation) wiped seq 0 → pos_max no longer covers
    // the matched prefix. Reuse MUST fall back, else we'd decode the delta
    // onto someone else's KV (silent corruption).
    Toks resident{10, 11, 12, 13};
    Toks incoming{10, 11, 12, 13, 20};
    CHECK(warm_keep_cut(resident, incoming, /*pos_max=*/-1) == 0u);  // empty KV
    CHECK(warm_keep_cut(resident, incoming, /*pos_max=*/1) == 0u);   // only 2 left
    CHECK(warm_keep_cut(resident, incoming, /*pos_max=*/3) == 4u);   // exactly enough
}

TEST_CASE("warm_keep_cut: degenerate inputs fall back",
          "[gh96][warm_keep]") {
    CHECK(warm_keep_cut(Toks{}, Toks{1, 2}, 5) == 0u);          // no resident
    CHECK(warm_keep_cut(Toks{1}, Toks{1}, 0) == 0u);            // incoming < 2
    CHECK(warm_keep_cut(Toks{1, 2}, Toks{1}, 1) == 0u);        // incoming < 2
}

TEST_CASE("warm_keep_cut: identical sequence still decodes the last token",
          "[gh96][warm_keep]") {
    // common == incoming.size(); must cap at size-1 so the final token's
    // logits are recomputed for sampling (never reuse the entire sequence).
    Toks seq{10, 11, 12, 13};
    CHECK(warm_keep_cut(seq, seq, /*pos_max=*/3) == 3u);  // size 4 → cut 3
}
