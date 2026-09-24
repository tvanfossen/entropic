// SPDX-License-Identifier: Apache-2.0
/**
 * @file partial_offload_test.cpp
 * @brief v2.12.0: the oversized-model GPU/CPU split, asserted without a GPU.
 *
 * The rule decides whether a model larger than the card runs slowly or dies,
 * so it is a pure function and this needs no device — same rationale as
 * warm_keep_util_test and session_pool_util_test.
 *
 * The case that motivated it: Qwen3.6-35B-A3B, 13.2 GB over 40 layers
 * (~330 MB each). A hardcoded 15 layers put ~5 GB on a card reporting 10.2 GB
 * free and left ~8.2 GB in system RAM, which the low-memory watchdog killed.
 *
 * @version 2.12.0
 */

#include "../../../src/inference/partial_offload.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

using entropic::partial_gpu_layers_for;
using entropic::kPartialOffloadReserveBytes;

namespace {
constexpr uint64_t kGiB = 1024ull * 1024 * 1024;
/// @brief The model that motivated this: 13.2 GB across 40 layers.
constexpr uint64_t kQwen36Bytes = 13211155424ull;
}  // namespace

SCENARIO("v2.12.0: the split follows free VRAM, not a hardcoded constant",
         "[partial_offload][2.12.0]") {
    GIVEN("the 13.2 GB / 40-layer model that OOMed the suite") {
        WHEN("the card is quiet, reporting 10.2 GB free") {
            const int layers =
                partial_gpu_layers_for(kQwen36Bytes, 10248ull * 1024 * 1024);

            THEN("far more than the old hardcoded 15 layers fit") {
                // ~330 MB/layer, 10.2 GB free less a 2 GB compute reserve
                // => ~8.2 GB => ~25 layers. The old constant left ~8.2 GB in
                // system RAM; this leaves roughly half that.
                INFO("layers chosen: " << layers);
                CHECK(layers > 15);
                CHECK(layers <= 40);
            }
        }

        WHEN("the desktop is holding VRAM, only 4 GB free") {
            const int layers =
                partial_gpu_layers_for(kQwen36Bytes, 4 * kGiB);

            THEN("it backs off rather than overcommitting the card") {
                // This is the v2.7.0 situation the constant was chosen for,
                // and the adaptive rule reaches a similar answer WITHOUT
                // assuming that state forever.
                INFO("layers chosen: " << layers);
                CHECK(layers > 0);
                CHECK(layers < 15);
            }
        }

        WHEN("free VRAM is below the compute reserve") {
            THEN("nothing is offloaded rather than a doomed allocation") {
                CHECK(partial_gpu_layers_for(kQwen36Bytes, kGiB) == 0);
                CHECK(partial_gpu_layers_for(
                          kQwen36Bytes, kPartialOffloadReserveBytes) == 0);
            }
        }

        WHEN("the card could hold the whole model") {
            THEN("the split is capped at the layer count, never beyond") {
                // Pass n_layers explicitly rather than leaning on the
                // default: the default is a deliberately conservative
                // ESTIMATE (see the header) and changing it must not silently
                // change what this asserts. An earlier cut hardcoded 40 here,
                // then failed when the default moved to 30 — the assertion
                // was testing the default, not the cap.
                CHECK(partial_gpu_layers_for(kQwen36Bytes, 64 * kGiB, 40)
                      == 40);
                CHECK(partial_gpu_layers_for(kQwen36Bytes, 64 * kGiB, 30)
                      == 30);
            }
        }
    }
}

SCENARIO("v2.12.0: an unknown layer count errs toward fewer GPU layers",
         "[partial_offload][2.12.0]") {
    GIVEN("a model whose real layer count is higher than assumed") {
        // Overestimating n_layers understates per-layer cost and computes
        // MORE layers than fit, overcommitting VRAM. Underestimating only
        // wastes capacity. The default must therefore sit at the low end.
        const int assumed_low = partial_gpu_layers_for(
            kQwen36Bytes, 10 * kGiB, 30);
        const int assumed_high = partial_gpu_layers_for(
            kQwen36Bytes, 10 * kGiB, 40);

        THEN("the lower assumption yields the safer, smaller split") {
            INFO("low=" << assumed_low << " high=" << assumed_high);
            CHECK(assumed_low <= assumed_high);
        }
    }
}

SCENARIO("v2.12.0: the WARM-load gate accounts for reload overlap",
         "[partial_offload][2.12.0]") {
    GIVEN("a 12.6 GB model") {
        const uint64_t file = 12599ull * 1024 * 1024;

        THEN("file plus a small constant is NOT enough to admit it") {
            // The first cut required file + 2 GB. It admitted this model at
            // ~15 GB available and the load then died at buffer allocation,
            // because WARM and ACTIVE residencies overlap across the reload.
            CHECK_FALSE(entropic::host_can_hold_warm_load(
                file, 15ull * 1024 * 1024 * 1024));
        }
        AND_THEN("ample memory still admits it") {
            CHECK(entropic::host_can_hold_warm_load(
                file, 32ull * 1024 * 1024 * 1024));
        }
        AND_THEN("an unmeasurable host is admitted rather than refused") {
            // Failing to measure is not evidence of scarcity.
            CHECK(entropic::host_can_hold_warm_load(file, 0));
        }
    }
}

SCENARIO("v2.12.0: degenerate inputs cannot produce a bad split",
         "[partial_offload][2.12.0]") {
    GIVEN("nonsense sizes") {
        THEN("they yield 0 rather than a negative or oversized layer count") {
            CHECK(partial_gpu_layers_for(0, 16 * kGiB) == 0);
            CHECK(partial_gpu_layers_for(kQwen36Bytes, 0) == 0);
            CHECK(partial_gpu_layers_for(kQwen36Bytes, 16 * kGiB, 0) == 0);
            CHECK(partial_gpu_layers_for(kQwen36Bytes, 16 * kGiB, -5) == 0);
        }
    }
}

namespace {

/// @brief Sets ENTROPIC_SKIP_LARGE_MODEL_TESTS for one scope and restores it.
///        Catch2 re-runs the enclosing GIVEN/WHEN once per leaf section, so an
///        unbalanced setenv here would leak the allowance into later sections
///        and into any test that runs after this file.
struct WaiverEnv {
    std::string prior;
    bool had_prior;
    explicit WaiverEnv(const char* value) {
        const char* p = std::getenv("ENTROPIC_SKIP_LARGE_MODEL_TESTS");
        had_prior = (p != nullptr);
        if (had_prior) { prior = p; }
        if (value == nullptr) {
            unsetenv("ENTROPIC_SKIP_LARGE_MODEL_TESTS");
        } else {
            setenv("ENTROPIC_SKIP_LARGE_MODEL_TESTS", value, 1);
        }
    }
    ~WaiverEnv() {
        if (had_prior) {
            setenv("ENTROPIC_SKIP_LARGE_MODEL_TESTS", prior.c_str(), 1);
        } else {
            unsetenv("ENTROPIC_SKIP_LARGE_MODEL_TESTS");
        }
    }
};

constexpr uint64_t kSmallModelBytes = 4ull * 1024 * 1024 * 1024;

}  // namespace

SCENARIO("v2.12.0: the large-model waiver cannot mute the whole suite",
         "[partial_offload][2.12.0]") {
    GIVEN("the operator allowance is set") {
        WaiverEnv env("1");

        THEN("a large model is waived") {
            CHECK(entropic::large_model_tests_waived(kQwen36Bytes));
        }
        // THE property. A waiver that applies to every model is not an
        // allowance, it is a mute button — one env var and a green suite
        // proves nothing. Every model in the suite bar two is under 6 GB.
        AND_THEN("an ordinary model is NOT waived") {
            CHECK_FALSE(entropic::large_model_tests_waived(kSmallModelBytes));
        }
        AND_THEN("the threshold itself is not waivable") {
            CHECK_FALSE(entropic::large_model_tests_waived(
                entropic::kLargeModelThresholdBytes));
        }
        AND_THEN("an unmeasurable size is not waived") {
            CHECK_FALSE(entropic::large_model_tests_waived(0));
        }
    }

    GIVEN("the allowance is unset") {
        WaiverEnv env(nullptr);
        THEN("even a large model runs — the waiver is opt-in") {
            CHECK_FALSE(entropic::large_model_tests_waived(kQwen36Bytes));
        }
    }

    GIVEN("the allowance is set to something other than 1") {
        WaiverEnv env("0");
        THEN("it does not count as set") {
            CHECK_FALSE(entropic::large_model_tests_waived(kQwen36Bytes));
        }
    }
}

// ── gh#148 (v2.13.0): typed refusals for unsafe explicit configs ──

SCENARIO("gh#148: use_mlock on an oversized model is refused, not flipped",
         "[partial_offload][gh148][2.13.0]") {
    constexpr uint64_t kLimit = 3ull * kGiB;   // a typical RLIMIT_MEMLOCK

    GIVEN("a 13.2 GB model partially offloaded with use_mlock left on") {
        WHEN("the lock would exceed RLIMIT_MEMLOCK") {
            THEN("the configuration is refused") {
                // v2.12.0 measured this: llama.cpp locks what it can, the
                // pinned pages cannot be reclaimed, and the suite was
                // OOM-KILLED rather than paging through the model. The
                // harness silently set use_mlock=false; the engine says so.
                CHECK(entropic::mlock_refused(true, kQwen36Bytes, 15, kLimit));
            }
        }

        WHEN("the operator already set use_mlock: false") {
            THEN("there is nothing to refuse") {
                CHECK_FALSE(
                    entropic::mlock_refused(false, kQwen36Bytes, 15, kLimit));
            }
        }

        WHEN("the model is fully offloaded") {
            THEN("the host side is transient and the rule does not apply") {
                CHECK_FALSE(
                    entropic::mlock_refused(true, kQwen36Bytes, -1, kLimit));
            }
        }

        WHEN("the limit is unlimited") {
            THEN("a gate with nothing to measure refuses nothing") {
                CHECK_FALSE(entropic::mlock_refused(
                    true, kQwen36Bytes, 15, entropic::kMemlockUnlimited));
            }
        }
    }

    GIVEN("an ordinary model") {
        // THE property. use_mlock DEFAULTS to true, so an un-floored rule
        // would refuse the everyday case of a CPU-resident 4 GB model on a
        // box with the usual small limit — a configuration that works.
        THEN("it is never refused, whatever the limit") {
            CHECK_FALSE(entropic::mlock_refused(true, kSmallModelBytes, 0,
                                                8ull * 1024 * 1024));
            CHECK_FALSE(entropic::mlock_refused(
                true, entropic::kLargeModelThresholdBytes, 15, kLimit));
        }
    }
}

SCENARIO("gh#148: an offload with no room for compute buffers is refused",
         "[partial_offload][gh148][2.13.0]") {
    GIVEN("a 13.2 GB model on a card with almost nothing free") {
        WHEN("free VRAM is below the compute reserve") {
            THEN("no positive layer count can work, so it is refused") {
                // Robust WITHOUT the model's layer count: if the compute
                // buffers alone do not fit, how the file divides into
                // layers cannot rescue it.
                CHECK(entropic::gpu_offload_refused(
                    kQwen36Bytes, 15, 1ull * kGiB));
                CHECK(entropic::gpu_offload_refused(
                    kQwen36Bytes, -1, entropic::kPartialOffloadReserveBytes));
            }
        }

        WHEN("the card has room") {
            THEN("the split is a judgement call, not a refusal") {
                // Deliberately NOT refused: judging a specific partial split
                // needs the layer count the gate does not read, and guessing
                // it refuses configurations that work.
                CHECK_FALSE(entropic::gpu_offload_refused(
                    kQwen36Bytes, 15, 10248ull * 1024 * 1024));
            }
        }

        WHEN("the tier is CPU-only") {
            THEN("there is no offload to refuse") {
                CHECK_FALSE(
                    entropic::gpu_offload_refused(kQwen36Bytes, 0, 1ull * kGiB));
            }
        }

        WHEN("free VRAM was never measured") {
            THEN("a gate that cannot measure refuses nothing") {
                CHECK_FALSE(
                    entropic::gpu_offload_refused(kQwen36Bytes, 15, 0));
            }
        }
    }

    GIVEN("an ordinary model on a busy card") {
        THEN("it is never refused — 1.5 GB is plenty for a 500 MB model") {
            CHECK_FALSE(entropic::gpu_offload_refused(
                kSmallModelBytes, 15, 1536ull * 1024 * 1024));
        }
    }
}
