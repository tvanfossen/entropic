// SPDX-License-Identifier: Apache-2.0
/**
 * @file batch_kv_util_test.cpp
 * @brief gh#158: a batch on one session must not wipe another's KV.
 *
 * @par What the RED was
 * `run_batched_decode` opened with `llama_memory_clear(mem, true)` — the
 * WHOLE cache — and gave its first arm sequence 0, which is a SESSION slot.
 * Both are invisible to a correctness test: a session whose KV was destroyed
 * still produces correct output on its next turn, having silently paid a
 * full cold prefill. So the observable has to be the CALL, not its effect —
 * the instrumentation shape #62/gh#161 and #65/gh#148 established.
 *
 * This file asserts the RULE (`plan_batch_kv`, which the production path
 * consults — a helper the code did not call would prove nothing). The
 * COUNTER half is `LlamaCppBackend::kv_full_clear_count()`, asserted across
 * a real batch by the gh#158 model test at the GPU gate.
 *
 * @version 2.13.0
 */

#include "../../../src/inference/batch_kv_util.h"
#include "../../../src/inference/session_pool_util.h"

#include <catch2/catch_test_macros.hpp>

using namespace entropic;

SCENARIO("gh#158: a batch never writes a sequence a session owns",
         "[inference][gh158][batch][2.13.0]") {
    GIVEN("a four-session pool and a three-request batch") {
        ModelConfig cfg;
        cfg.context_length = 4096;
        cfg.max_sessions = 4;
        cfg.n_parallel = 1;
        const int slots = derive_pool_geometry(cfg).temp_seq_base;
        REQUIRE(slots == 4);

        WHEN("the temp pool hands back ids that start below the slots") {
            // Exactly the pre-gh#158 shape: allocate_temp_seq_id() mints
            // 1, 2, 3..., and arm 0 was hardcoded to sequence 0.
            const auto plan = plan_batch_kv(3, slots, {1, 2, 3});

            THEN("every arm is lifted clear of the session range") {
                REQUIRE(plan.seq_ids.size() == 3);
                for (int id : plan.seq_ids) { CHECK(id >= slots); }
            }
            AND_THEN("the arms are distinct — no two share a sequence") {
                CHECK(plan.seq_ids[0] != plan.seq_ids[1]);
                CHECK(plan.seq_ids[1] != plan.seq_ids[2]);
                CHECK(plan.seq_ids[0] != plan.seq_ids[2]);
            }
            AND_THEN("the plan disturbs no session") {
                CHECK_FALSE(plan_disturbs_sessions(plan, slots));
            }
        }

        WHEN("the temp pool already hands back ids above the slots") {
            const auto plan = plan_batch_kv(3, slots, {4, 5, 6});

            THEN("they are used verbatim") {
                CHECK(plan.seq_ids == std::vector<int>{4, 5, 6});
                CHECK_FALSE(plan_disturbs_sessions(plan, slots));
            }
        }
    }
}

SCENARIO("gh#158: a batch never clears the whole KV cache",
         "[inference][gh158][batch][2.13.0]") {
    GIVEN("any batch plan") {
        const auto plan = plan_batch_kv(2, 1, {1, 2});

        THEN("it does not claim the right to clear everything") {
            // The field exists so the property is ASSERTED rather than
            // assumed: a future change that brings the whole-cache clear
            // back has to set it, and this fails.
            CHECK_FALSE(plan.clears_whole_cache);
        }
        AND_THEN("a plan that DID clear everything would be caught") {
            BatchKvPlan bad = plan;
            bad.clears_whole_cache = true;
            CHECK(plan_disturbs_sessions(bad, 1));
        }
    }
}

SCENARIO("gh#158: the single-session default is unchanged",
         "[inference][gh158][batch][2.13.0]") {
    GIVEN("no session pool configured — temp_seq_base is 1") {
        ModelConfig cfg;
        cfg.context_length = 4096;
        cfg.max_sessions = 1;
        cfg.n_parallel = 4;  // the gh#98 fan-out shape
        const int slots = derive_pool_geometry(cfg).temp_seq_base;
        REQUIRE(slots == 1);

        WHEN("a four-request batch is planned") {
            const auto plan = plan_batch_kv(4, slots, {1, 2, 3, 4});

            THEN("sequence 0 is still left alone") {
                // Pre-gh#158 arm 0 took sequence 0 unconditionally, which is
                // slot 0's session even when only one session exists.
                for (int id : plan.seq_ids) { CHECK(id != 0); }
                CHECK_FALSE(plan_disturbs_sessions(plan, slots));
            }
        }
    }
}
