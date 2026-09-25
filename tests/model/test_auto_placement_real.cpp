// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_auto_placement_real.cpp
 * @brief `gpu_layers: auto` against real GGUFs on a real card (v2.13.1).
 *
 * The CPU tests for `auto_placement.h` feed it synthetic shapes. That proves
 * the arithmetic and nothing about whether `read_gguf_shape` reads the field
 * it thinks it reads, or whether the placement it derives is the one the
 * model actually runs at. VRAM math is unfalsifiable on CPU: a wrong estimate
 * surfaces as a failed allocation on a device, never as a failing unit test.
 *
 * So this asserts the two things only a real file and a real card can answer:
 *
 *   1. The shape read from the GGUF matches what the model reports once
 *      loaded — layer count, and expert count for a MoE.
 *   2. The placement `auto` derives for the 26B-A4B at IQ2 is FULL
 *      residency, because v2.13.0 measured that configuration running at
 *      30 of 30 layers. v2.13.0's estimator derives 26 for the same inputs,
 *      so this fails on the old code and passes on the new — the property
 *      the whole change exists for.
 *
 * @version 2.13.2
 */
#include <catch2/catch_test_macros.hpp>

#include "auto_placement.h"
#include "gguf_metadata.h"
#include "model_test_context.h"

#include <filesystem>

namespace fs = std::filesystem;

using entropic::AutoPlacement;
using entropic::derive_auto_placement;
using entropic::FootprintInputs;
using entropic::GgufShape;
using entropic::read_gguf_shape;

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

/**
 * @brief Locate a bundled GGUF by filename, or "" when absent.
 * @param name GGUF filename.
 * @return Path, or empty when the model is not downloaded.
 * @internal
 * @version 2.13.1
 */
fs::path model_file(const std::string& name) {
    const fs::path p = fs::path(getenv("HOME")) / ".entropic" / "models" / name;
    return fs::is_regular_file(p) ? p : fs::path{};
}

}  // namespace

SCENARIO("the GGUF shape read matches what the model really is",
         "[model][auto_placement][2.13.1]") {
    GIVEN("the 26B-A4B IQ2 on disk") {
        const auto path = model_file("gemma-4-26B-A4B-it-UD-IQ2_M.gguf");
        if (path.empty()) {
            SKIP("gemma-4-26B-A4B-it-UD-IQ2_M.gguf not present");
        }

        WHEN("its metadata is read without loading tensors") {
            const GgufShape shape = read_gguf_shape(path.string());

            THEN("the shape is usable and describes a 30-layer MoE") {
                INFO("arch=" << shape.architecture
                     << " blocks=" << shape.block_count
                     << " experts=" << shape.expert_count
                     << " block_bytes=" << shape.block_bytes / kMiB
                     << " expert_bytes=" << shape.expert_bytes / kMiB
                     << " non_block=" << shape.non_block_bytes / kMiB);
                REQUIRE(shape.known);
                // The v2.13.0 estimator ASSUMED 30 and happened to be right
                // for this model. The point is that it is now read, not
                // assumed — a 42-layer E4B was priced as 30 (gh#192).
                CHECK(shape.block_count == 30);
                CHECK(shape.expert_count > 0);
            }
            AND_THEN("expert tensors are a real, dominant share of a block") {
                // Summed from tensors matching llama.cpp's expert pattern.
                // If the pattern stopped matching after a pin bump this goes
                // to zero and an expert split would silently free nothing.
                REQUIRE(shape.block_bytes > 0);
                CHECK(shape.expert_bytes > 0);
                CHECK(shape.expert_bytes < shape.block_bytes);
                const double share =
                    static_cast<double>(shape.expert_bytes)
                    / static_cast<double>(shape.block_bytes);
                INFO("experts are " << (share * 100.0) << "% of block bytes");
                CHECK(share > 0.5);
            }
            AND_THEN("the block bytes account for most of the file") {
                std::error_code ec;
                const auto file_bytes = fs::file_size(path, ec);
                REQUIRE_FALSE(ec);
                const uint64_t summed =
                    shape.block_bytes + shape.non_block_bytes;
                INFO("summed=" << summed / kMiB
                     << " MiB file=" << file_bytes / kMiB << " MiB");
                // Tensor data plus a small header/metadata remainder.
                CHECK(summed <= file_bytes);
                CHECK(summed > file_bytes * 9 / 10);
            }
        }
    }
}

SCENARIO("auto derives full residency for a model measured to fit",
         "[model][auto_placement][2.13.1]") {
    GIVEN("the 26B-A4B IQ2 and this card's free VRAM") {
        const auto path = model_file("gemma-4-26B-A4B-it-UD-IQ2_M.gguf");
        if (path.empty()) {
            SKIP("gemma-4-26B-A4B-it-UD-IQ2_M.gguf not present");
        }
        const GgufShape shape = read_gguf_shape(path.string());
        REQUIRE(shape.known);

        // v2.13.0 ran this configuration at 30 of 30 layers: plain 52.46,
        // mtp 61.05 tok/s, with roughly 500 MiB spare. Its own estimator
        // derives 26 for the same inputs.
        std::error_code ec;
        const auto file_bytes = fs::file_size(path, ec);
        REQUIRE_FALSE(ec);

        FootprintInputs in;
        in.weights_bytes = static_cast<uint64_t>(file_bytes);
        in.context_length = 8192;
        in.cache_type_k = "q4_0";
        in.cache_type_v = "q4_0";
        in.vram_reserve_mb = 512;
        in.draft_bytes = 225 * kMiB;  // mtp_a4b

        WHEN("auto is asked to place it on an idle 11 GiB card") {
            // The bench card as the measured run saw it.
            const uint64_t free_vram = 10518 * kMiB;
            const AutoPlacement p =
                derive_auto_placement(shape, in, free_vram, 128);

            THEN("it keeps every layer resident") {
                INFO("reason: " << p.reason
                     << " gpu_layers=" << p.gpu_layers
                     << " cpu_moe=" << p.cpu_moe_layers
                     << " est=" << p.bytes / kMiB << " MiB");
                REQUIRE(p.known);
                CHECK(p.fully_resident);
                // -1, not 30 — the sentinel is what keeps the mlock
                // exemption and the estimator's full branch.
                CHECK(p.gpu_layers == -1);
            }
            AND_THEN("the estimate is under the free VRAM it was given") {
                CHECK(p.bytes <= free_vram);
            }
        }

        // v2.13.2. The 128 above is hand-fed, and that is exactly the
        // configuration management `auto` exists to remove: an operator who
        // sets nothing gets llama.cpp's 512, and THIS model at 512 is the
        // run that died at the compute buffer (weights 9552 MiB fine, KV 84,
        // compute 527, against ~10465 free). Full residency was never out of
        // reach — one number was.
        WHEN("auto is asked with no n_ubatch configured at all") {
            const uint64_t free_vram = 10518 * kMiB;
            const AutoPlacement p = derive_auto_placement(shape, in, free_vram, 0);

            THEN("it still keeps every layer resident") {
                INFO("reason: " << p.reason
                     << " gpu_layers=" << p.gpu_layers
                     << " n_ubatch=" << p.n_ubatch
                     << " est=" << p.bytes / kMiB << " MiB");
                REQUIRE(p.known);
                CHECK(p.fully_resident);
                CHECK(p.cpu_moe_layers == 0);
            }
            AND_THEN("it paid for that with ubatch, not with layers") {
                // Measured on this card, four iterations, freeing the same
                // ~1800 MiB either way: ubatch costs prefill alone (decode
                // error bars overlap); layers cost prefill 44% AND decode
                // 66%. Layers are the two-axis currency, so ubatch goes
                // first — and only as far as it must (256 before 128).
                CHECK(p.n_ubatch > 0);
                CHECK(p.n_ubatch < 512);
                CHECK(p.n_ubatch >= 128);
            }
        }
    }
}
