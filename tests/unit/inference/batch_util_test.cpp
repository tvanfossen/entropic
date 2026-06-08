// SPDX-License-Identifier: Apache-2.0
/**
 * @file batch_util_test.cpp
 * @brief CPU unit tests for the gh#98 shared-prefix batch helper.
 *
 * @version 2.8.0
 */

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "batch_util.h"

using entropic::batch_is_viable;
using entropic::batch_shared_prefix_len;

namespace {
using Seqs = std::vector<std::vector<int>>;
}

TEST_CASE("batch_shared_prefix_len: fewer than 2 sequences → 0",
          "[inference][batch_util]") {
    CHECK(batch_shared_prefix_len(Seqs{}) == 0);
    CHECK(batch_shared_prefix_len(Seqs{{1, 2, 3}}) == 0);
}

TEST_CASE("batch_shared_prefix_len: common prefix, capped one below shortest",
          "[inference][batch_util]") {
    // Shared run is [9,9,9,9] (len 4); shortest seq is len 6 → cap at 5,
    // but the actual common prefix (4) is below the cap, so 4 wins.
    Seqs seqs{
        {9, 9, 9, 9, 1, 2},
        {9, 9, 9, 9, 3, 4, 5},
        {9, 9, 9, 9, 7, 8, 9, 0},
    };
    CHECK(batch_shared_prefix_len(seqs) == 4);
}

TEST_CASE("batch_shared_prefix_len: identical sequences cap at shortest-1",
          "[inference][batch_util]") {
    // Fully identical → common prefix is the whole length (4); the cap
    // (shortest-1 = 3) forces a non-empty suffix on every sequence.
    Seqs seqs{{5, 5, 5, 5}, {5, 5, 5, 5}};
    CHECK(batch_shared_prefix_len(seqs) == 3);
}

TEST_CASE("batch_shared_prefix_len: no common first token → 0",
          "[inference][batch_util]") {
    Seqs seqs{{1, 2, 3}, {4, 5, 6}};
    CHECK(batch_shared_prefix_len(seqs) == 0);
}

TEST_CASE("batch_shared_prefix_len: any empty sequence → 0",
          "[inference][batch_util]") {
    Seqs seqs{{1, 2, 3}, {}};
    CHECK(batch_shared_prefix_len(seqs) == 0);
}

TEST_CASE("batch_shared_prefix_len: single-token shortest → 0 (needs a suffix)",
          "[inference][batch_util]") {
    Seqs seqs{{7}, {7, 8, 9}};
    CHECK(batch_shared_prefix_len(seqs) == 0);
}

TEST_CASE("batch_shared_prefix_len: realistic same-prefix NPC batch",
          "[inference][batch_util]") {
    // ~shared constitution [100..104] then per-NPC suffix tokens.
    std::vector<int> prefix{100, 101, 102, 103, 104};
    Seqs seqs;
    for (int s : {11, 22, 33, 44}) {
        auto seq = prefix;
        seq.push_back(s);
        seq.push_back(s + 1);
        seqs.push_back(seq);
    }
    CHECK(batch_shared_prefix_len(seqs) == 5);
}

// ── batch_is_viable: the fast-path guard (gh#98) ──────────────

TEST_CASE("batch_is_viable: viable when all conditions hold",
          "[inference][batch_util]") {
    // 8 requests, n_parallel=8, shared=700, plain KV, suffixes 8*45=360, n_batch=2048.
    CHECK(batch_is_viable(8, 8, 700, /*hybrid=*/false, 360, 2048));
}

TEST_CASE("batch_is_viable: hybrid arch is never batched (seq ops unsafe)",
          "[inference][batch_util]") {
    // Same viable inputs but hybrid=true → MUST fall back to serial (gh#97).
    CHECK_FALSE(batch_is_viable(8, 8, 700, /*hybrid=*/true, 360, 2048));
}

TEST_CASE("batch_is_viable: fewer than 2 requests → serial",
          "[inference][batch_util]") {
    CHECK_FALSE(batch_is_viable(1, 8, 700, false, 45, 2048));
}

TEST_CASE("batch_is_viable: no shared prefix (disjoint) → serial",
          "[inference][batch_util]") {
    CHECK_FALSE(batch_is_viable(8, 8, 0, false, 6000, 2048));
}

TEST_CASE("batch_is_viable: batch exceeds sequence slots → serial",
          "[inference][batch_util]") {
    // 16 requests but only 8 seq slots configured.
    CHECK_FALSE(batch_is_viable(16, 8, 700, false, 720, 2048));
}

TEST_CASE("batch_is_viable: suffix tokens overrun the decode batch → serial",
          "[inference][batch_util]") {
    // total_suffix 4096 > n_batch 2048.
    CHECK_FALSE(batch_is_viable(8, 8, 700, false, 4096, 2048));
}
