// SPDX-License-Identifier: Apache-2.0
/**
 * @file partial_offload_reserve.h
 * @brief Force a TRUE partial offload on a model that fits the card (v2.14.6).
 *
 * The pared model set has no model larger than the 11 GB dev card, so the
 * only way left to exercise `gpu_layers: auto`'s partial-offload branch on
 * real hardware is to shrink the budget it places against. The engine's own
 * lever for that is `vram_reserve_mb` (`ENTROPIC_VRAM_RESERVE_MB`): the
 * reserve is added to every footprint estimate, so a placement fits only when
 * `resident + KV + reserve <= free VRAM`.
 *
 * This header derives that reserve from the device and the GGUF instead of a
 * pinned number, and checks the result with the engine's own solver
 * (`derive_auto_placement`) before any model is loaded — so a test built on it
 * fails before a load if the window it relies on is not there.
 *
 * The window, for a MoE: `auto` prefers "every layer resident, experts
 * host-side" over dropping layers, so a layer count below `n_layer` appears
 * only once even ALL experts host-side does not fit. The budget is therefore
 * aimed between
 *   - lo: two whole layers on the card (the smallest partial placement this
 *     accepts), and
 *   - hi: every layer resident with every expert host-side (the next
 *     placement up, which must NOT fit),
 * at their midpoint, so drift in free VRAM between this measurement and the
 * engine's own does not move the placement out of the partial branch.
 *
 * @version 2.14.6
 */

#pragma once

#include "auto_placement.h"
#include "gguf_metadata.h"
#include "vram_footprint.h"

#include <ggml-backend.h>

#include <cstdint>

namespace entropic::test {

/// @brief One GPU device's memory as ggml reports it. @version 2.14.6
struct DeviceVram {
    uint64_t free = 0;   ///< Free bytes; 0 when no GPU device is registered
    uint64_t total = 0;  ///< Total bytes; 0 when no GPU device is registered
};

/**
 * @brief Free and total VRAM of the first GPU device ggml reports.
 *
 * The same query the engine's admission gate makes
 * (`query_device_free_vram_bytes`), plus the total it does not expose.
 *
 * @return Both figures, zero when there is no GPU device.
 * @utility
 * @version 2.14.6
 */
inline DeviceVram query_device_vram() {
    DeviceVram v;
    ggml_backend_dev_t dev =
        ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);
    if (dev != nullptr) {
        size_t free_bytes = 0;
        size_t total_bytes = 0;
        ggml_backend_dev_memory(dev, &free_bytes, &total_bytes);
        v.free = free_bytes;
        v.total = total_bytes;
    }
    return v;
}

/// @brief A reserve that forces partial offload, and the evidence for it.
/// @version 2.14.6
struct PartialReserve {
    bool ok = false;          ///< The window exists and the solver agrees
    int reserve_mb = 0;       ///< Value for `vram_reserve_mb`
    uint64_t lo_bytes = 0;    ///< Resident bytes of 2 whole layers (+ draft)
    uint64_t hi_bytes = 0;    ///< Resident bytes, all experts host-side (+ draft)
    uint64_t kv_bytes = 0;    ///< KV term the estimator charges
    uint64_t target_bytes = 0;  ///< Resident budget aimed for: (lo + hi) / 2
    AutoPlacement predicted;  ///< What the engine's solver derives with it
};

/**
 * @brief Choose `vram_reserve_mb` so `auto` resolves to 0 < gpu_layers < n.
 *
 * @param shape The model's GGUF shape.
 * @param base Footprint inputs as the engine builds them for the tier
 *        (`footprint_inputs_for`), with `draft_bytes` set when an MTP head
 *        is configured. Its `vram_reserve_mb` is ignored.
 * @param free_vram Free VRAM now — the figure the engine places against.
 * @param n_ubatch The tier's n_ubatch (0 = llama.cpp default).
 * @return The reserve and the solver's prediction; `ok` false when the card
 *         or model leaves no room for a two-layer-margin window.
 * @utility
 * @version 2.14.6
 */
inline PartialReserve choose_partial_reserve(const GgufShape& shape,
                                             FootprintInputs base,
                                             uint64_t free_vram,
                                             int n_ubatch) {
    constexpr uint64_t kMiB = 1024ull * 1024ull;
    PartialReserve r;
    const uint64_t blocks =
        shape.block_count > 0 ? static_cast<uint64_t>(shape.block_count) : 1;
    const uint64_t per_block = shape.block_bytes / blocks;
    base.vram_reserve_mb = 0;
    FootprintInputs kv_only = base;
    kv_only.gpu_layers = 0;  // resident 0 + draft + KV
    r.kv_bytes = estimate_vram_footprint(kv_only).bytes - base.draft_bytes;
    r.lo_bytes = 2 * per_block + base.draft_bytes;
    r.hi_bytes = base.weights_bytes - shape.expert_bytes + base.draft_bytes;
    r.target_bytes = (r.lo_bytes + r.hi_bytes) / 2;
    const bool window = shape.known && r.hi_bytes > r.lo_bytes + 2 * per_block
        && free_vram > r.kv_bytes + r.target_bytes;
    if (window) {
        r.reserve_mb = static_cast<int>(
            (free_vram - r.kv_bytes - r.target_bytes) / kMiB);
        base.vram_reserve_mb = r.reserve_mb;
        r.predicted = derive_auto_placement(shape, base, free_vram, n_ubatch);
        r.ok = r.predicted.known && !r.predicted.fully_resident
            && r.predicted.gpu_layers > 0
            && r.predicted.gpu_layers < shape.block_count;
    }
    return r;
}

}  // namespace entropic::test
