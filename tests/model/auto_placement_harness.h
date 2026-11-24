// SPDX-License-Identifier: Apache-2.0
/**
 * @file auto_placement_harness.h
 * @brief `gpu_layers: auto` for model tests that drive the backend directly.
 *
 * The orchestrator resolves `auto` at admission. A test that builds a
 * LlamaCppBackend itself never passes through admission, so a hardcoded
 * gpu_layers was all it had, and the test machine's VRAM is not knowable in
 * advance. This resolves the placement with the orchestrator's own
 * derivation (derive_tier_auto_placement) against VRAM measured now, and
 * logs it. v2.14.6, user decision: "resolve auto in the test properly".
 *
 * @version 2.14.6
 */

#pragma once

#include <entropic/types/config.h>

#include "../../src/inference/device_memory.h"
#include "../../src/inference/tier_placement.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

/**
 * @brief Resolve `gpu_layers: auto` for a backend-direct test config.
 *
 * Writes gpu_layers, cpu_moe_layers and (when auto lowered it) n_ubatch into
 * @p cfg, exactly as ModelOrchestrator::resolve_auto_gpu_layers writes them
 * into a tier. Uses the default vram_reserve_mb and no draft head.
 *
 * @param cfg Backend config to place; unchanged when placement fails.
 * @param label Test label for the log line.
 * @return true when a placement was derived and applied; false (logged at
 *         ERROR, with the reason) when the file, the device or the shape
 *         could not be read, or nothing fits. The caller must fail on false:
 *         a test that cannot place its model has not run.
 * @utility
 * @version 2.14.6
 */
inline bool resolve_auto_for_backend_test(entropic::ModelConfig& cfg,
                                          const std::string& label) {
    entropic::TierConfig tier;
    static_cast<entropic::ModelConfig&>(tier) = cfg;
    std::error_code ec;
    const auto file_bytes = std::filesystem::file_size(cfg.path, ec);
    const uint64_t free_vram = entropic::query_device_free_vram_bytes();
    entropic::AutoPlacement placement;
    entropic::GgufShape shape;
    if (!ec && free_vram > 0) {
        placement = entropic::derive_tier_auto_placement(
            tier, static_cast<uint64_t>(file_bytes), free_vram,
            entropic::ParsedConfig{}.vram_reserve_mb, 0, shape);
    }
    if (!placement.known) {
        spdlog::error("[{}] gpu_layers=auto could not be resolved (file {}, "
                      "free VRAM {} MiB): {}", label,
                      ec ? "unreadable" : "ok", free_vram / (1024ULL * 1024),
                      placement.reason);
        return false;
    }
    cfg.gpu_layers = placement.gpu_layers;
    cfg.cpu_moe_layers = placement.cpu_moe_layers;
    if (placement.n_ubatch > 0) { cfg.n_ubatch = placement.n_ubatch; }
    spdlog::info("[{}] gpu_layers=auto -> gpu_layers={} cpu_moe_layers={} "
                 "n_ubatch={} (free VRAM {} MiB, estimate {} MiB, block_count "
                 "{})", label, cfg.gpu_layers, cfg.cpu_moe_layers, cfg.n_ubatch,
                 free_vram / (1024ULL * 1024),
                 placement.bytes / (1024ULL * 1024), shape.block_count);
    return true;
}
