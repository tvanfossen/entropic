// SPDX-License-Identifier: Apache-2.0
/**
 * @file seq_id_pool_test.cpp
 * @brief gh#98 (v2.8.0): the temp seq_id pool must hand out DISTINCT ids.
 *
 * The multi-seq batched decode allocates N temp seq_ids back-to-back (one per
 * request) with NO intervening release, then `seq_cp`s the shared prefix into
 * each and fills batch cells tagged by seq_id. If two sequences share a seq_id
 * they share KV slots → cross-contamination. The pre-fix allocator returned
 * `1 + free_seq_ids_.size()`, which is 1 on EVERY empty-pool call, so the whole
 * batch collided on seq_id 1. A forcing grammar makes the model output
 * independent of the (corrupted) KV, so the GPU batch test passed vacuously —
 * this CPU test is the genuine regression guard.
 *
 * @version 2.8.0
 */

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

#include "llama_cpp_backend.h"

using entropic::LlamaCppBackend;

TEST_CASE("temp seq_id pool hands out distinct ids on an empty pool (gh#98)",
          "[inference][seq_id][gh98]") {
    LlamaCppBackend backend;

    // Allocate 8 ids back-to-back (the multi-seq batch pattern: no release
    // between allocations). They MUST all be distinct.
    std::vector<llama_seq_id> ids;
    for (int i = 0; i < 8; ++i) {
        ids.push_back(backend.allocate_temp_seq_id_for_test());
    }
    std::set<llama_seq_id> uniq(ids.begin(), ids.end());
    CHECK(uniq.size() == ids.size());        // all distinct (RED on `1+size()`)
    CHECK(uniq.count(0) == 0);               // 0 is reserved for generation
}

TEST_CASE("released temp seq_ids are reused, keeping the high-water bounded "
          "(gh#98)", "[inference][seq_id][gh98]") {
    LlamaCppBackend backend;

    auto a = backend.allocate_temp_seq_id_for_test();
    auto b = backend.allocate_temp_seq_id_for_test();
    CHECK(a != b);

    backend.release_temp_seq_id_for_test(b);
    // The released id is reused before a fresh one is minted.
    auto c = backend.allocate_temp_seq_id_for_test();
    CHECK(c == b);

    // And a brand-new allocation past the pool stays distinct from the live one.
    auto d = backend.allocate_temp_seq_id_for_test();
    CHECK(d != a);
    CHECK(d != c);
}
