// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file auto_placement_test.cpp
 * @brief `gpu_layers: auto` derives a placement that actually fits (v2.13.1).
 *
 * The numbers here are the ones v2.13.0 measured on a GTX 1080 Ti, so a
 * regression shows up as a disagreement with a real run rather than with a
 * made-up budget.
 *
 * @version 2.13.1
 */
#include <catch2/catch_test_macros.hpp>

#include "auto_placement.h"
#include "partial_offload.h"

using entropic::AutoPlacement;
using entropic::derive_auto_placement;
using entropic::FootprintInputs;
using entropic::GgufShape;

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

/// @brief The 26B-A4B at IQ2 as v2.13.1 reads it. @internal @version 2.13.1
GgufShape a4b_iq2() {
    GgufShape s;
    s.known = true;
    s.architecture = "gemma4moe";
    s.block_count = 30;
    s.expert_count = 128;
    // Measured: 9535.79 MiB of model buffer. Experts dominate a MoE layer.
    s.block_bytes = 9100 * kMiB;
    s.expert_bytes = 7600 * kMiB;
    s.non_block_bytes = 436 * kMiB;
    return s;
}

/// @brief A dense 42-layer E4B-shaped model. @internal @version 2.13.1
GgufShape e4b_dense() {
    GgufShape s;
    s.known = true;
    s.architecture = "gemma4";
    s.block_count = 42;
    s.expert_count = 0;
    s.block_bytes = 6800 * kMiB;
    s.non_block_bytes = 400 * kMiB;
    return s;
}

/// @brief The ubatch the measured A4B IQ2 run used (bench tier, e4382f5).
constexpr int kMeasuredUbatch = 128;

/// @brief Inputs for the 1080 Ti runs.
///
/// `weights_bytes` MUST be set: the estimator's FULL branch prices from it,
/// while the partial branch prices from `block_bytes`. A fixture that left it
/// zero made every fully-resident candidate cost nothing and therefore always
/// fit — which is how a test can pass while asserting nothing.
/// @internal @version 2.13.1
FootprintInputs bench_inputs(const GgufShape& shape) {
    FootprintInputs in;
    in.weights_bytes = shape.block_bytes + shape.non_block_bytes;
    in.context_length = 8192;
    in.cache_type_k = "q4_0";
    in.cache_type_v = "q4_0";
    in.max_sessions = 1;
    in.vram_reserve_mb = 512;
    in.draft_bytes = 225 * kMiB;  // mtp_a4b, measured
    return in;
}

}  // namespace

SCENARIO("auto keeps a model fully resident when it fits",
         "[inference][auto_placement][2.13.1]") {
    GIVEN("the A4B IQ2 that v2.13.0 measured at 30 of 30 layers resident") {
        // This is the case that proves the old estimator wrong: the run
        // happened, at gpu_layers -1, with roughly 500 MiB spare. The old
        // `file_bytes / 30` arithmetic against a flat 2 GiB reserve derives
        // 26 and leaves four layers on the host for no reason.
        const auto shape = a4b_iq2();
        // The card as the BENCH RUN saw it: 11162 MiB total less ~644 MiB of
        // desktop. Deliberately not the 10379 MiB from a consumer's failure
        // log — that was a differently-loaded card on another occasion, and
        // modelling one run with another's free-VRAM figure is how a test
        // ends up asserting something that never happened. The run this
        // asserts used ~10397 MiB and had roughly 120 MiB spare.
        const uint64_t free_vram = 10518 * kMiB;

        WHEN("auto derives a placement") {
            const AutoPlacement p =
                derive_auto_placement(shape, bench_inputs(shape), free_vram,
                                      kMeasuredUbatch);

            THEN("every layer is resident and no experts are displaced") {
                INFO("reason: " << p.reason << " bytes: " << p.bytes / kMiB);
                REQUIRE(p.known);
                // -1, not 30: the sentinel is what the engine reads as "all
                // layers". A concrete count would price this through the
                // PARTIAL branch (omitting embeddings and the output head)
                // and strip the mlock exemption, which keys on gpu_layers<0.
                CHECK(p.fully_resident);
                CHECK(p.gpu_layers == -1);
                CHECK(p.cpu_moe_layers == 0);
            }
        }
    }
}

SCENARIO("the v2.13.0 estimator is measurably worse on the same inputs",
         "[inference][auto_placement][2.13.1]") {
    GIVEN("the A4B IQ2 and the free VRAM its real run reported") {
        // This is the RED evidence, kept permanently rather than deleted
        // once green: it pins what the shipped estimator actually answers,
        // so the improvement is a measured difference and reverting the fix
        // fails a test instead of quietly restoring the old number.
        const auto shape = a4b_iq2();
        const uint64_t free_vram = 10379 * kMiB;
        const uint64_t file_bytes = 9550 * kMiB;

        WHEN("both estimators are asked") {
            const int old_answer = entropic::partial_gpu_layers_for(
                file_bytes, free_vram);
            const AutoPlacement now =
                derive_auto_placement(shape, bench_inputs(shape), free_vram,
                                      kMeasuredUbatch);

            THEN("the old one leaves layers on the host that demonstrably fit") {
                INFO("v2.13.0: " << old_answer << " layers | v2.13.1: "
                     << now.gpu_layers << " layers | measured run: 30");
                REQUIRE(now.known);
                // The run happened at 30 of 30 with roughly 500 MiB spare.
                CHECK(now.fully_resident);
                CHECK(old_answer < 30);
            }
        }
    }
}

SCENARIO("auto sheds experts before it sheds a layer",
         "[inference][auto_placement][2.13.1]") {
    GIVEN("the same MoE on a card too small to hold all of it") {
        // Moving a layer off takes its ATTENTION with it; moving that
        // layer's experts off frees most of the bytes and keeps attention
        // resident. v2.13.0 measured the difference: 22.62 tok/s with
        // experts host-side against 18.86 for whole-layer offload.
        const auto shape = a4b_iq2();
        const uint64_t free_vram = 8000 * kMiB;

        WHEN("auto derives a placement") {
            const AutoPlacement p =
                derive_auto_placement(shape, bench_inputs(shape), free_vram,
                                      kMeasuredUbatch);

            THEN("all layers stay on the card, experts move to the host") {
                INFO("reason: " << p.reason << " gpu_layers: " << p.gpu_layers
                     << " cpu_moe: " << p.cpu_moe_layers);
                REQUIRE(p.known);
                CHECK(p.fully_resident);
                CHECK(p.cpu_moe_layers > 0);
            }
            AND_THEN("it takes no more experts than it needs") {
                CHECK(p.cpu_moe_layers < 30);
            }
        }
    }
}

SCENARIO("auto prices a dense model by its own layer count, not by 30",
         "[inference][auto_placement][2.13.1]") {
    GIVEN("a 42-layer dense model that fits") {
        // The gh#192 defect: resolve_auto_gpu_layers never passed the real
        // count, so a 42-layer model was priced as if it had 30 — per-layer
        // cost overstated by 40%, and a model that fits gets clipped.
        const auto shape = e4b_dense();
        const uint64_t free_vram = 10379 * kMiB;

        WHEN("auto derives a placement") {
            FootprintInputs in = bench_inputs(shape);
            in.draft_bytes = 0;
            const AutoPlacement p = derive_auto_placement(shape, in, free_vram, kMeasuredUbatch);

            THEN("all 42 layers are resident") {
                INFO("reason: " << p.reason << " gpu_layers: " << p.gpu_layers);
                REQUIRE(p.known);
                CHECK(p.fully_resident);
            }
        }
    }
}

SCENARIO("auto budgets the KV cache, so context changes the answer",
         "[inference][auto_placement][2.13.1]") {
    GIVEN("a model that fits at 8k") {
        const auto shape = a4b_iq2();
        const uint64_t free_vram = 10379 * kMiB;
        FootprintInputs small = bench_inputs(shape);

        WHEN("the same model is asked for a very large context") {
            // The old estimator priced weights only, against a flat 2 GiB
            // reserve that did not move with context_length. A KV cache that
            // outgrows the reserve was invisible to it.
            FootprintInputs large = bench_inputs(shape);
            large.context_length = 131072;

            const AutoPlacement a =
                derive_auto_placement(shape, small, free_vram, kMeasuredUbatch);
            const AutoPlacement b =
                derive_auto_placement(shape, large, free_vram, kMeasuredUbatch);

            THEN("the larger context yields a smaller resident placement") {
                INFO("8k: layers=" << a.gpu_layers << " moe=" << a.cpu_moe_layers
                     << " | 128k: layers=" << b.gpu_layers
                     << " moe=" << b.cpu_moe_layers);
                REQUIRE(a.known);
                const bool b_is_smaller =
                    !b.known || (a.fully_resident && !b.fully_resident)
                    || b.cpu_moe_layers > a.cpu_moe_layers;
                CHECK(b_is_smaller);
            }
        }
    }
}

SCENARIO("auto holds back room for the compute buffers nobody can price",
         "[inference][auto_placement][2.13.1]") {
    GIVEN("the same model and card at two different ubatch sizes") {
        // The footprint estimator explicitly does not count graph scratch:
        // it is sized by ubatch and model internals, and a speculative
        // configuration pays it twice for two contexts. Auto is deciding on
        // the operator's behalf, so it holds room back — and holds back MORE
        // when the ubatch is larger, because that is what the cost tracks.
        const auto shape = a4b_iq2();
        const uint64_t free_vram = 10379 * kMiB;

        WHEN("the ubatch quadruples") {
            const AutoPlacement small =
                derive_auto_placement(shape, bench_inputs(shape), free_vram, 128);
            const AutoPlacement large =
                derive_auto_placement(shape, bench_inputs(shape), free_vram, 512);

            THEN("the larger ubatch yields a more conservative placement") {
                INFO("ubatch 128: layers=" << small.gpu_layers
                     << " moe=" << small.cpu_moe_layers
                     << " | ubatch 512: layers=" << large.gpu_layers
                     << " moe=" << large.cpu_moe_layers);
                REQUIRE(small.known);
                const bool more_conservative =
                    !large.known
                    || (small.fully_resident && !large.fully_resident)
                    || large.cpu_moe_layers > small.cpu_moe_layers;
                CHECK(more_conservative);
            }
        }
    }
}

SCENARIO("auto declines rather than guessing when the shape is unreadable",
         "[inference][auto_placement][2.13.1]") {
    GIVEN("a shape that could not be read") {
        GgufShape unknown;  // known == false
        const uint64_t free_vram = 10379 * kMiB;

        WHEN("auto is asked to derive a placement") {
            const AutoPlacement p =
                derive_auto_placement(unknown, bench_inputs(a4b_iq2()), free_vram,
                                      kMeasuredUbatch);

            THEN("it reports that it cannot decide") {
                CHECK_FALSE(p.known);
                CHECK(std::string(p.reason).find("unknown")
                      != std::string::npos);
            }
        }
    }

    GIVEN("a device reporting no free VRAM") {
        WHEN("auto is asked to derive a placement") {
            const AutoPlacement p =
                derive_auto_placement(a4b_iq2(), bench_inputs(a4b_iq2()), 0,
                                      kMeasuredUbatch);

            THEN("it declines rather than deriving zero layers") {
                CHECK_FALSE(p.known);
            }
        }
    }
}
