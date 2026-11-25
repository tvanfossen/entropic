// SPDX-License-Identifier: Apache-2.0
/**
 * @file tier_placement.h
 * @brief One tier's footprint inputs and its `gpu_layers: auto` placement.
 *
 * v2.14.6: moved out of orchestrator.cpp (where footprint_inputs_for was a
 * file-static) so a backend-direct model test can resolve `auto` exactly
 * as the orchestrator does. A second, test-side estimator would be the
 * defect decision 81 forbids: two estimators disagreeing about one tier.
 *
 * @version 2.14.6
 */

#pragma once

#include <entropic/types/config.h>

#include "auto_placement.h"
#include "gguf_metadata.h"
#include "vram_footprint.h"

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace entropic {

/**
 * @brief Gather a tier's footprint inputs for the pure estimator.
 *
 * Resolves the vision projector's size when the tier declares one — it is the
 * allocation that actually aborted the process in gh#142 and the v2.2.4
 * estimate ignored it entirely.
 *
 * @param tier_cfg The tier's configuration.
 * @param weights_bytes Size of the tier's GGUF on disk.
 * @param vram_reserve_mb Headroom the operator reserved.
 * @return Inputs for estimate_vram_footprint.
 * @utility
 * @version 2.14.6
 */
inline FootprintInputs footprint_inputs_for(
    const TierConfig& tier_cfg, uint64_t weights_bytes, int vram_reserve_mb) {
    FootprintInputs in;
    in.weights_bytes = weights_bytes;
    in.gpu_layers = tier_cfg.gpu_layers;
    in.context_length = tier_cfg.context_length;
    in.cache_type_k = tier_cfg.cache_type_k;
    in.cache_type_v = tier_cfg.cache_type_v;
    in.vram_reserve_mb = vram_reserve_mb;
    // gh#144 (v2.12.0): context_length is per session, so the pool's KV term
    // multiplies. Omitting this under-counts by exactly N and can admit a
    // configuration that aborts the host — what this gate exists to prevent.
    in.max_sessions = tier_cfg.max_sessions;
    if (!tier_cfg.mmproj_path.empty()) {
        std::error_code proj_ec;
        auto proj = std::filesystem::file_size(tier_cfg.mmproj_path, proj_ec);
        if (!proj_ec) { in.mmproj_bytes = proj; }
    }
    // v2.13.1: the GGUF's real shape, and what an expert split moves off the
    // card. Without these the budget gate re-prices a placement `auto` just
    // derived WITHOUT knowing about the split: it sees gpu_layers == -1,
    // calls that full residency, charges the whole file, and refuses a
    // configuration auto had already measured as fitting. Two estimators
    // disagreeing about one tier is the defect this release exists to
    // remove, so both go through this builder and both see the same shape.
    const GgufShape shape = read_gguf_shape(tier_cfg.path);
    if (shape.known) {
        in.block_count = shape.block_count;
        in.block_bytes = shape.block_bytes;
        in.non_block_bytes = shape.non_block_bytes;
        if (tier_cfg.cpu_moe_layers > 0) {
            in.host_expert_bytes =
                shape.expert_bytes_per_block()
                * static_cast<uint64_t>(tier_cfg.cpu_moe_layers);
        }
    }
    return in;
}

/**
 * @brief Derive one tier's `gpu_layers: auto` placement (v2.14.6).
 *
 * The orchestrator's admission step and backend-direct model tests both
 * call this, so they price a tier with the same function that will judge it.
 *
 * @param tier_cfg The tier's configuration.
 * @param file_bytes Size of the tier's GGUF on disk.
 * @param free_vram_bytes Free VRAM on the target device right now.
 * @param vram_reserve_mb Headroom the operator reserved.
 * @param draft_bytes A resident draft head's bytes (0 for none).
 * @param shape_out Receives the GGUF shape read, for the caller's log line.
 * @return The placement; `known == false` when nothing usable was found.
 * @req REQ-INFER-019
 * @version 2.14.6
 */
inline AutoPlacement derive_tier_auto_placement(
    const TierConfig& tier_cfg, uint64_t file_bytes, uint64_t free_vram_bytes,
    int vram_reserve_mb, uint64_t draft_bytes, GgufShape& shape_out) {
    shape_out = read_gguf_shape(tier_cfg.path.string());
    FootprintInputs base =
        footprint_inputs_for(tier_cfg, file_bytes, vram_reserve_mb);
    base.draft_bytes = draft_bytes;
    return derive_auto_placement(shape_out, base, free_vram_bytes,
                                 tier_cfg.n_ubatch);
}

} // namespace entropic
