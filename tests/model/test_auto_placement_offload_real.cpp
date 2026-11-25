// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_auto_placement_offload_real.cpp
 * @brief `gpu_layers: auto` partial offload and refusal on a REAL load (v2.14.6).
 *
 * Until v2.14.6 the partial-offload path ran for real only because the dev
 * card could not hold the 26B-A4B QAT Q4. That model left the pared set, and
 * the A4B that replaced it (UD-IQ2_M, 9.33 GiB) fits the card whole — so
 * without a lever, nothing in the gate would ever place a model partially.
 *
 * The lever is the engine's own VRAM reserve (`vram_reserve_mb`). It is added
 * to every footprint estimate, so it shrinks the budget a placement must fit
 * without needing a bigger model or a busier card. Both cases derive it from
 * the device and the GGUF (partial_offload_reserve.h), never from a pinned
 * card size, and check it against the engine's own solver before loading:
 *
 *   1. A reserve aimed between "two whole layers" and "every layer with every
 *      expert host-side" must make auto resolve to 0 < gpu_layers < n_layer —
 *      read from the engine's own placement log line — load, generate
 *      coherently, and keep the process's device memory inside
 *      total - reserve (plus a stated tolerance).
 *   2. A reserve equal to the whole card cannot fit even zero layers' KV, so
 *      the tier must be REFUSED with a typed error — not crash, and not load
 *      anything on the CPU without saying so.
 *
 * One TEST_CASE per ctest entry (add_model_test_per_case), so each load runs
 * in a fresh process (gh#142 — VRAM does not return within a process).
 *
 * @version 2.14.6
 */

#include <catch2/catch_test_macros.hpp>

#include "model_test_context.h"
#include "partial_offload_reserve.h"

#include <entropic/inference/orchestrator.h>
#include <entropic/types/config.h>
#include <entropic/types/message.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr char kModel[] = "gemma-4-26B-A4B-it-UD-IQ2_M.gguf";
constexpr int kContext = 4096;

/// @brief CUDA context, cuBLAS workspace and allocator slack not priced by
///        the estimator. Measured: a CUDA primary context is ~138 MiB on the
///        GTX 1080 Ti (device_memory.h, gh#198); 512 MiB covers that plus the
///        workspace with room to spare.
constexpr uint64_t kCudaSlackBytes = 512ull * kMiB;

/**
 * @brief Locate the model, SKIPping only when it is not downloaded.
 * @return Path to the GGUF.
 * @internal
 * @version 2.14.6
 */
fs::path model_or_skip() {
    const char* home = std::getenv("HOME");
    const fs::path p = home != nullptr
        ? fs::path(home) / ".entropic" / "models" / kModel : fs::path{};
    if (!fs::is_regular_file(p)) {
        SKIP(std::string(kModel) + " not present at " + p.string());
    }
    return p;
}

/**
 * @brief One auto-placed tier on the A4B, nothing else configured.
 * @param path GGUF path.
 * @param reserve_mb `vram_reserve_mb` for the handle.
 * @return Parsed config for ModelOrchestrator::initialize.
 * @internal
 * @version 2.14.6
 */
entropic::ParsedConfig offload_config(const fs::path& path, int reserve_mb) {
    entropic::ParsedConfig config;
    config.vram_reserve_mb = reserve_mb;
    config.models.default_tier = "lead";
    entropic::TierConfig lead;
    lead.path = path;
    lead.adapter = "gemma4";
    lead.context_length = kContext;
    lead.gpu_layers = -1;
    lead.gpu_layers_auto = true;
    lead.flash_attn = false;
    // Partial offload leaves most of the file host-side; pinning it would
    // make the load fatal instead of slow (v2.12.0).
    lead.use_mlock = false;
    config.models.tiers["lead"] = lead;
    return config;
}

/**
 * @brief Footprint inputs the way the engine builds them for this tier.
 *
 * Mirrors `footprint_inputs_for` (orchestrator.cpp): weights, context,
 * cache types, sessions, no projector, no draft head.
 *
 * @param tier The tier as configured.
 * @param shape The model's GGUF shape.
 * @param file_bytes GGUF size on disk.
 * @return Inputs for the pure solver.
 * @internal
 * @version 2.14.6
 */
entropic::FootprintInputs inputs_for(const entropic::TierConfig& tier,
                                     const entropic::GgufShape& shape,
                                     uint64_t file_bytes) {
    entropic::FootprintInputs in;
    in.weights_bytes = file_bytes;
    in.gpu_layers = tier.gpu_layers;
    in.context_length = tier.context_length;
    in.cache_type_k = tier.cache_type_k;
    in.cache_type_v = tier.cache_type_v;
    in.max_sessions = tier.max_sessions;
    in.block_count = shape.block_count;
    in.block_bytes = shape.block_bytes;
    in.non_block_bytes = shape.non_block_bytes;
    return in;
}

/**
 * @brief Does the answer name a primary colour (RGB or RYB)?
 * @param text Model output.
 * @return true when it does.
 * @internal
 * @version 2.14.6
 */
bool names_primary_colour(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    bool found = false;
    for (const char* c : {"red", "blue", "yellow", "green"}) {
        found = found || text.find(c) != std::string::npos;
    }
    return found;
}

}  // namespace

TEST_CASE("gh#148 auto: a VRAM reserve forces a real partial offload that "
          "stays inside its budget",
          "[model][auto_placement][offload]") {
    const fs::path path = model_or_skip();
    const entropic::GgufShape shape = entropic::read_gguf_shape(path.string());
    REQUIRE(shape.known);
    const uint64_t file_bytes = fs::file_size(path);
    wait_for_host_memory(static_cast<long>(file_bytes / kMiB) + 2048);

    // Sampled before anything loads: the after-minus-before delta is this
    // load's own device memory. The first query also creates this process's
    // CUDA context, so the context is outside the delta.
    const auto before = entropic::test::query_device_vram();
    INFO("this is a GPU gate: ggml reports no GPU device");
    REQUIRE(before.total > 0);

    auto config = offload_config(path, 0);
    const auto r = entropic::test::choose_partial_reserve(
        shape, inputs_for(config.models.tiers["lead"], shape, file_bytes),
        before.free, 0);
    INFO("free=" << before.free / kMiB << " total=" << before.total / kMiB
         << " MiB; window lo=" << r.lo_bytes / kMiB << " hi="
         << r.hi_bytes / kMiB << " target=" << r.target_bytes / kMiB
         << " kv=" << r.kv_bytes / kMiB << " MiB; reserve=" << r.reserve_mb
         << " MiB; solver predicts " << r.predicted.gpu_layers << " of "
         << shape.block_count << " (" << r.predicted.reason << ")");
    REQUIRE(r.ok);
    config.vram_reserve_mb = r.reserve_mb;

    const std::string log = "auto_partial_offload";
    start_test_log(log);
    entropic::ModelOrchestrator orch;
    REQUIRE(orch.initialize(config));

    std::vector<entropic::Message> msgs;
    msgs.push_back({"system", "You are a terse assistant. Answer in one word."});
    msgs.push_back({"user", "Name one primary colour."});
    entropic::GenerationParams params;
    params.max_tokens = 32;
    params.temperature = 0.0f;
    params.enable_thinking = false;
    const auto result = orch.generate(msgs, params, "lead");

    const auto after = entropic::test::query_device_vram();
    // log_auto_placement: "[residency] tier 'lead': gpu_layers=auto -> K of
    // N layers — partial offload (...)".
    const long layers = test_log_int_after(log, "gpu_layers=auto -> ");
    const bool partial_line = test_log_contains(log, "— partial offload (");
    end_test_log();
    orch.shutdown();

    const uint64_t used = before.free > after.free ? before.free - after.free : 0;
    const uint64_t reserve = static_cast<uint64_t>(r.reserve_mb) * kMiB;
    // The reserve is what the operator asked to keep free, and vram_footprint.h
    // names covering compute buffers as its purpose — but the buffers are
    // allocated INSIDE this process's delta. Tolerance: the engine's own
    // per-context compute figure at ubatch 512 plus the CUDA slack above.
    const uint64_t tolerance = entropic::kComputeBytesAtUbatch512 + kCudaSlackBytes;
    const uint64_t bound = before.total - reserve + tolerance;
    INFO("auto resolved " << layers << " of " << shape.block_count
         << " layers; device memory taken by this load " << used / kMiB
         << " MiB; bound total - reserve + tolerance = "
         << before.total / kMiB << " - " << r.reserve_mb << " + "
         << tolerance / kMiB << " = " << bound / kMiB << " MiB; model "
         << file_bytes / kMiB << " MiB\nanswer=[" << result.content << "]");

    CHECK(partial_line);
    CHECK(layers > 0);
    CHECK(layers < shape.block_count);
    CHECK(result.error_code == 0);
    CHECK_FALSE(result.content.empty());
    CHECK(names_primary_colour(result.content));
    CHECK(used <= bound);
}

TEST_CASE("gh#148 auto: a reserve no placement can fit is refused with a "
          "typed error",
          "[model][auto_placement][offload]") {
    const fs::path path = model_or_skip();
    const entropic::GgufShape shape = entropic::read_gguf_shape(path.string());
    REQUIRE(shape.known);
    const uint64_t file_bytes = fs::file_size(path);

    const auto dev = entropic::test::query_device_vram();
    INFO("this is a GPU gate: ggml reports no GPU device");
    REQUIRE(dev.total > 0);

    // The whole card as the reserve: reserve alone exceeds free VRAM, so not
    // even zero layers (KV only) fits, at any ubatch.
    const int reserve_mb = static_cast<int>(dev.total / kMiB);
    auto config = offload_config(path, reserve_mb);
    auto in = inputs_for(config.models.tiers["lead"], shape, file_bytes);
    in.vram_reserve_mb = reserve_mb;
    const auto p = entropic::derive_auto_placement(shape, in, dev.free, 0);
    INFO("reserve=" << reserve_mb << " MiB, free=" << dev.free / kMiB
         << " MiB; solver: known=" << p.known << " (" << p.reason << ")");
    REQUIRE_FALSE(p.known);  // the engine's own solver agrees nothing fits

    const std::string log = "auto_offload_refused";
    start_test_log(log);
    entropic::ModelOrchestrator orch;
    const bool initialized = orch.initialize(config);
    const auto code = orch.last_residency_error();
    const auto loaded = orch.loaded_models();
    // residency_admits: "[residency] tier 'lead' footprint X bytes exceeds
    // VRAM budget Y bytes — TIER_MODEL_TOO_LARGE (gh#57)".
    const bool typed_line =
        test_log_contains(log, "TIER_MODEL_TOO_LARGE (gh#57)");
    end_test_log();

    INFO("initialize=" << initialized << " code=" << code
         << " loaded=" << loaded.size());
    CHECK_FALSE(initialized);
    CHECK(code == ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE);
    CHECK(typed_line);
    // No silent fallback: nothing was loaded, on the GPU or the CPU.
    CHECK(loaded.empty());
}
