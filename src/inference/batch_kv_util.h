// SPDX-License-Identifier: Apache-2.0
/**
 * @file batch_kv_util.h
 * @brief gh#158: which sequences a same-prefix batch may touch, purely.
 *
 * @par The bug this encodes the fix for
 * `run_batched_decode` opened with `llama_memory_clear(mem, true)` — the
 * WHOLE KV cache, every sequence, not only the ones the batch is about to
 * write — and followed it with `invalidate_all_resident_kv()`. With one run
 * per handle that was merely wasteful: nobody else held cells. Once runs are
 * keyed per session (gh#158), a batch issued by session A destroys session
 * B's resident prefix and B's next turn silently pays a full cold prefill
 * having been told nothing.
 *
 * It also took sequence 0 for its own first arm, which IS a session slot —
 * so the damage did not even need the whole-cache clear to happen.
 *
 * @par Why a pure header
 * The same reason `session_pool_util.h` and `warm_keep_util.h` are: the rule
 * is integer bookkeeping that decides whether one caller can corrupt
 * another's state, and it must be assertable by a CPU unit test with no
 * model, no GPU and no `llama_context`. `run_batched_decode` APPLIES this
 * plan; it does not decide it. A test that asserted against a helper the
 * production path did not consult would prove nothing.
 *
 * @version 2.13.0
 */

#pragma once

#include <cstddef>
#include <vector>

namespace entropic {

/**
 * @brief Which sequences a batch will write, and what it may clear.
 * @version 2.13.0
 */
struct BatchKvPlan {
    /// @brief Sequence ids the batch will write, one per request.
    std::vector<int> seq_ids;

    /// @brief Whether the batch may clear the WHOLE KV cache.
    ///
    /// Always false from v2.13.0. Kept as a named field rather than deleted
    /// so the property is asserted rather than assumed — a future change
    /// that reintroduces the whole-cache clear has to set it, and the test
    /// that forbids it fails.
    bool clears_whole_cache = false;
};

/**
 * @brief Plan the sequence ids a same-prefix batch may use.
 *
 * Every arm — including the first — is given a sequence at or above
 * `session_slots`, so no arm can land on a slot a session holds. Before
 * gh#158 arm 0 was hardcoded to sequence 0, which is slot 0's session.
 *
 * @param n Request count.
 * @param session_slots Sequences reserved for the session pool
 *        (`PoolGeometry::temp_seq_base`; 1 when no pool is configured).
 * @param temp_ids Sequence ids minted by the backend's temp pool, in order.
 * @return The plan the batch must follow.
 * @req REQ-INFER-018
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
inline BatchKvPlan plan_batch_kv(std::size_t n, int session_slots,
                                 const std::vector<int>& temp_ids) {
    BatchKvPlan plan;
    plan.clears_whole_cache = false;
    plan.seq_ids.reserve(n);
    const int floor_id = session_slots < 1 ? 1 : session_slots;
    for (std::size_t i = 0; i < n && i < temp_ids.size(); ++i) {
        // Defence in depth: a temp pool whose base was never raised past the
        // session slots would otherwise hand back a session's sequence. The
        // configure-time exclusion in session_pool_util.h makes that
        // unreachable today; this makes it unreachable by construction.
        plan.seq_ids.push_back(
            temp_ids[i] < floor_id ? floor_id + static_cast<int>(i)
                                   : temp_ids[i]);
    }
    return plan;
}

/**
 * @brief Whether a plan can disturb any session's resident KV.
 *
 * The property the gh#158 regression test asserts, stated once here so the
 * test and any future caller ask the same question.
 *
 * @param plan Plan to check.
 * @param session_slots Sequences reserved for the session pool.
 * @return true when the plan would clear the whole cache or write a
 *         sequence a session owns.
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
inline bool plan_disturbs_sessions(const BatchKvPlan& plan,
                                   int session_slots) {
    if (plan.clears_whole_cache) { return true; }
    bool disturbs = false;
    for (int id : plan.seq_ids) {
        if (id < session_slots) { disturbs = true; }
    }
    return disturbs;
}

}  // namespace entropic
