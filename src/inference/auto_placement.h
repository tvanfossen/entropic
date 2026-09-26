// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file auto_placement.h
 * @brief Derive a model's placement from its shape and the free VRAM.
 *
 * `gpu_layers: auto` exists so that fitting a new model is a decision the
 * engine makes rather than a number an operator tunes. Until v2.13.1 it made
 * that decision badly: `partial_gpu_layers_for` priced every model as thirty
 * layers, charged `file_bytes / 30` per layer — folding in embeddings and the
 * output head, which do not move with `gpu_layers` — and reserved a flat
 * 2 GiB for everything that is not weights, double-counting against the
 * `vram_reserve_mb` the real estimator already honours. It knew nothing about
 * the KV cache, the draft head, or experts. On the 26B-A4B at IQ2 it derives
 * 26 of 30 layers for a model measured to run at 30 of 30 with headroom to
 * spare.
 *
 * This solves the estimator instead of approximating beside it. There is one
 * footprint model in the engine (`vram_footprint.h`); auto asks it which
 * placements fit and takes the best one.
 *
 * **Order of concessions.** When everything does not fit, a MoE gives up its
 * experts before it gives up a layer. Moving one layer off the card takes its
 * attention with it, and attention is what speculative decode and every
 * prefill leans on; moving that layer's *experts* off frees the bulk of its
 * bytes and leaves the attention resident. That ordering is why v2.13.0
 * measured expert offload at 22.62 tok/s against 18.86 for whole-layer
 * offload of the same model.
 *
 * @version 2.13.1
 */
#pragma once

#include "gguf_metadata.h"
#include "vram_footprint.h"

#include <cstdint>

namespace entropic {

/**
 * @brief A placement decision, with the reason it was reached.
 * @version 2.13.1
 */
struct AutoPlacement {
    bool known = false;       ///< False = could not decide; leave config alone.

    /// @brief Layers to offload, or -1 meaning ALL.
    ///
    /// Full residency is reported as the -1 sentinel, not as a count equal
    /// to `block_count`, because -1 is what the rest of the engine reads as
    /// "all layers": `classify_offload` prices it as fully resident, and
    /// `mlock_refused` exempts it (`gpu_layers < 0`) since the host side is
    /// then transient rather than pinned. Emitting a concrete 30 would
    /// silently price a fully-resident model through the PARTIAL branch —
    /// omitting embeddings and the output head, an under-count — and strip
    /// the mlock exemption from a configuration that works today.
    int gpu_layers = 0;
    int cpu_moe_layers = 0;   ///< Layers whose experts stay host-side.
    uint64_t bytes = 0;       ///< Estimated resident footprint of the choice.
    bool fully_resident = false;  ///< True when `gpu_layers` is the -1 sentinel.

    /// @brief n_ubatch auto chose, or 0 to leave the operator's value alone.
    ///
    /// v2.13.2: auto gives up ubatch before it gives up a layer. Measured on
    /// a 26B-A4B at IQ2, four iterations, freeing the same ~1800 MiB either
    /// way: lowering ubatch costs prefill 49% and decode NOTHING (overlapping
    /// error bars); dropping six layers costs prefill 44% and decode 66%.
    /// Layers are the two-axis currency, so they are spent last.
    int n_ubatch = 0;

    /// @brief Free VRAM left over once this placement is loaded, in bytes.
    ///
    /// v2.13.2. `bytes` says what the placement costs; this says what it
    /// does NOT consume, which is the number that decides whether the
    /// placement survives ordinary growth — a longer system prompt, one
    /// more tool schema, a KV change. A consumer put it exactly right: a
    /// placement that fits by fifteen megabytes is a coincidence, not a
    /// configuration. The engine takes it (it was measured running) and
    /// states the margin so the operator can decide otherwise.
    uint64_t margin_bytes = 0;
    const char* reason = "";  ///< Why this placement, for the operator's log.
};

/// @brief Measured compute scratch for ONE context at a 512-token ubatch
///        (vram_footprint.h: 1222 MiB, gemma-4 E4B with an MTP head).
constexpr uint64_t kComputeBytesAtUbatch512 = 1222ull * 1024ull * 1024ull;

/// @brief The ubatch that figure was measured at.
constexpr int kComputeReferenceUbatch = 512;

/// @brief llama.cpp's default ubatch, assumed when the tier does not set one.
constexpr int kDefaultUbatch = 512;

/// @brief Values auto will step down through, descending.
///
/// config.h calls 128, 256 and 512 the productive values; below 128 the
/// prefill cost keeps rising while the VRAM freed flattens, so there is
/// nothing useful further down.
constexpr int kUbatchLadder[] = {512, 256, 128};

/**
 * @brief VRAM to hold back for graph/activation scratch.
 *
 * The footprint estimator does not count compute buffers and documents why:
 * they are sized by ubatch and model internals, not by anything the estimate
 * reads. A speculative configuration holds TWO contexts and pays the cost
 * twice.
 *
 * Held back by `auto` specifically. An operator naming `gpu_layers` has
 * decided for themselves; auto has not, so it must leave room for the term
 * it cannot compute.
 *
 * **Scaled by ubatch, because that is what the cost tracks.** Two
 * measurements from this project anchor it: 1222 MiB per context for a
 * gemma-4 E4B MTP head at ubatch 512, and roughly 520 MiB across all
 * contexts for the 26B-A4B at IQ2 at ubatch 128. A flat allowance sized for
 * the first refuses the second — which is a configuration this project has
 * measured running at 30 of 30 layers with headroom to spare. Refusing a
 * placement that demonstrably works is the failure this whole change exists
 * to stop, so the allowance moves with the knob the cost moves with.
 *
 * @param base Footprint inputs — `draft_bytes` non-zero means two contexts.
 * @param n_ubatch Physical batch size; <= 0 assumes llama.cpp's default.
 * @return Bytes to subtract from free VRAM before choosing a placement.
 * @dg_internal
 * @version 2.13.1
 */
inline uint64_t compute_allowance_bytes(const FootprintInputs& base,
                                        int n_ubatch) {
    const int ubatch = n_ubatch > 0 ? n_ubatch : kDefaultUbatch;
    const uint64_t contexts = base.draft_bytes > 0 ? 2ull : 1ull;
    const uint64_t scaled =
        kComputeBytesAtUbatch512 * static_cast<uint64_t>(ubatch)
        / static_cast<uint64_t>(kComputeReferenceUbatch) * contexts;
    // Only the EXCESS over `vram_reserve_mb`. That reserve is already inside
    // the estimate and vram_footprint.h names covering compute buffers as
    // its whole purpose, so holding back the full figure on top of it would
    // charge the same bytes twice — the exact defect this change exists to
    // remove, reintroduced one layer up. At the default 512 MB reserve and a
    // 128-token ubatch the two nearly cancel, which is why the 26B-A4B at
    // IQ2 still comes out fully resident, as measured.
    const uint64_t reserve =
        static_cast<uint64_t>(base.vram_reserve_mb) * 1024ull * 1024ull;
    return scaled > reserve ? scaled - reserve : 0ull;
}

/**
 * @brief Does a candidate placement fit the budget?
 * @param base Footprint inputs describing everything except placement.
 * @param shape The model's GGUF shape.
 * @param gpu_layers Candidate layer count.
 * @param cpu_moe_layers Candidate expert-offload count.
 * @param budget_bytes VRAM the placement must fit inside.
 * @param[out] used Estimated bytes for this candidate.
 * @return True when the estimate is known and within budget.
 * @dg_internal
 * @version 2.13.1
 */
inline bool placement_fits(const FootprintInputs& base,
                           const GgufShape& shape,
                           int gpu_layers,
                           int cpu_moe_layers,
                           uint64_t budget_bytes,
                           uint64_t* used) {
    FootprintInputs in = base;
    // A candidate naming every layer IS full residency; hand the estimator
    // the sentinel so it takes the full branch and counts the embeddings
    // and output head, which the partial branch deliberately excludes.
    in.gpu_layers =
        (gpu_layers < 0 || gpu_layers >= shape.block_count) ? -1 : gpu_layers;
    in.block_count = shape.block_count;
    in.block_bytes = shape.block_bytes;
    in.non_block_bytes = shape.non_block_bytes;
    in.host_expert_bytes =
        shape.expert_bytes_per_block() * static_cast<uint64_t>(cpu_moe_layers);
    const FootprintEstimate est = estimate_vram_footprint(in);
    if (used != nullptr) { *used = est.bytes; }
    return est.known && est.bytes <= budget_bytes;
}

/**
 * @brief Try the best case: every layer and every expert on the card.
 * @param shape Model shape.
 * @param base Footprint inputs.
 * @param budget VRAM budget.
 * @param[out] out Filled in on success.
 * @return True when it fits.
 * @dg_internal
 * @version 2.13.1
 */
inline bool try_fully_resident(const GgufShape& shape,
                               const FootprintInputs& base,
                               uint64_t budget, AutoPlacement* out) {
    uint64_t used = 0;
    if (!placement_fits(base, shape, shape.block_count, 0, budget, &used)) {
        return false;
    }
    out->known = true;
    out->gpu_layers = -1;  // the sentinel, not a count — see AutoPlacement.
    out->fully_resident = true;
    out->bytes = used;
    out->reason = "fully resident";
    return true;
}

/**
 * @brief Try keeping every layer resident by moving experts host-side.
 *
 * A MoE only, and tried BEFORE dropping layers: moving a layer off takes
 * its attention with it, while moving that layer's experts off frees the
 * bulk of its bytes and leaves attention on the card.
 *
 * @param shape Model shape.
 * @param base Footprint inputs.
 * @param budget VRAM budget.
 * @param[out] out Filled in on success.
 * @return True when some expert split fits.
 * @dg_internal
 * @version 2.13.1
 */
inline bool try_expert_offload(const GgufShape& shape,
                               const FootprintInputs& base,
                               uint64_t budget, AutoPlacement* out) {
    if (shape.expert_count <= 0 || shape.expert_bytes == 0) { return false; }
    uint64_t used = 0;
    for (int moe = 1; moe <= shape.block_count; ++moe) {
        if (!placement_fits(base, shape, shape.block_count, moe,
                            budget, &used)) {
            continue;
        }
        out->known = true;
        out->gpu_layers = -1;  // all layers; experts are what moved.
        out->fully_resident = true;
        out->cpu_moe_layers = moe;
        out->bytes = used;
        out->reason = "all layers resident, experts host-side";
        return true;
    }
    return false;
}

/**
 * @brief Last resort: take the largest layer count that fits.
 * @param shape Model shape.
 * @param base Footprint inputs.
 * @param budget VRAM budget.
 * @param[out] out Filled in on success.
 * @return True when any layer count fits.
 * @dg_internal
 * @version 2.13.1
 */
inline bool try_fewer_layers(const GgufShape& shape,
                             const FootprintInputs& base,
                             uint64_t budget, AutoPlacement* out) {
    uint64_t used = 0;
    for (int layers = shape.block_count - 1; layers >= 0; --layers) {
        if (!placement_fits(base, shape, layers, 0, budget, &used)) {
            continue;
        }
        out->known = true;
        out->gpu_layers = layers;
        out->bytes = used;
        out->reason = "partial offload";
        return true;
    }
    return false;
}

/**
 * @brief Try full residency at each ubatch below the configured one.
 *
 * Tried BEFORE displacing experts or layers, and stopping at the FIRST value
 * that fits rather than descending to a floor — "as far as needed" not "as
 * far as possible". A prefill-heavy workload should lose the minimum, and
 * 128 when 256 would have fit is a 2x prefill loss nobody asked for.
 *
 * @param shape Model shape.
 * @param base Footprint inputs.
 * @param free_vram_bytes Whole-device free VRAM (the allowance is per step).
 * @param configured Operator's n_ubatch; 0 means llama.cpp's default.
 * @param[out] out Filled in on success, including the chosen n_ubatch.
 * @return True when some reduced ubatch fits everything.
 * @dg_internal
 * @version 2.13.2
 */
inline bool try_smaller_ubatch(const GgufShape& shape,
                               const FootprintInputs& base,
                               uint64_t free_vram_bytes, int configured,
                               AutoPlacement* out) {
    const int current = configured > 0 ? configured : kDefaultUbatch;
    for (const int ub : kUbatchLadder) {
        if (ub >= current) { continue; }
        const uint64_t allowance = compute_allowance_bytes(base, ub);
        const uint64_t budget =
            free_vram_bytes > allowance ? free_vram_bytes - allowance : 0;
        if (!try_fully_resident(shape, base, budget, out)) { continue; }
        out->n_ubatch = ub;
        out->reason = "fully resident at a reduced n_ubatch";
        return true;
    }
    return false;
}

/**
 * @brief Choose the placement that uses the card best without overcommitting.
 *
 * Tries, in order of preference:
 *   1. Everything resident.
 *   2. Everything resident at a smaller `n_ubatch` (v2.13.2) — measured to
 *      cost prefill only, where every option below it costs decode too.
 *   3. Everything resident with experts progressively moved host-side — a
 *      MoE only, and preferred over dropping layers because it keeps
 *      attention on the card.
 *   4. Progressively fewer layers, for a dense model or when even all
 *      experts host-side is not enough.
 *
 * @param shape GGUF shape from `read_gguf_shape`.
 * @param base Footprint inputs: context, cache types, sessions, mmproj,
 *             draft head, and `vram_reserve_mb` headroom. Its `gpu_layers`
 *             is ignored — that is what this decides.
 * @param free_vram_bytes Free VRAM on the target device right now.
 * @param n_ubatch Tier's physical batch size; 0 assumes llama.cpp's
 *                default. Auto may return a SMALLER one in
 *                `AutoPlacement::n_ubatch`; it never returns a larger.
 * @return The chosen placement; `known == false` when the shape is unusable
 *         or nothing fits, and the caller must then leave the configuration
 *         as the operator wrote it.
 * @req REQ-INFER-019
 * @version 2.13.2
 */
inline AutoPlacement derive_auto_placement(const GgufShape& shape,
                                           const FootprintInputs& base,
                                           uint64_t free_vram_bytes,
                                           int n_ubatch = 0) {
    AutoPlacement out;
    if (!shape.known || free_vram_bytes == 0) {
        out.reason = "GGUF shape or free VRAM unknown";
        return out;
    }
    // The headroom the operator asked for is already inside the estimate
    // (vram_reserve_mb), so it is not subtracted twice — that double-count
    // is what v2.13.0 shipped.
    //
    // What IS subtracted is a compute-buffer allowance, because the
    // estimator explicitly does not count graph/activation scratch and says
    // so: llama.cpp sizes it by ubatch and model internals, and
    // vram_footprint.h measured 1222 MiB for one gemma-4 E4B MTP context at
    // a 512-token ubatch — more than twice the 512 MB default reserve, and
    // paid TWICE by a speculative configuration holding two contexts.
    //
    // Auto is choosing on the operator's behalf, so it must leave room for
    // the thing it cannot price rather than hand back a placement that fits
    // the estimate and fails the load. This is an allowance, not a
    // derivation; it is deliberately generous, and the cost of being wrong
    // is a layer left on the host rather than a failed load.
    const uint64_t allowance = compute_allowance_bytes(base, n_ubatch);
    const uint64_t budget =
        free_vram_bytes > allowance ? free_vram_bytes - allowance : 0;
    // Order is the measurement: ubatch costs ONE axis, experts and layers
    // cost two. Spend the cheap currency first.
    if (!try_fully_resident(shape, base, budget, &out)
        && !try_smaller_ubatch(shape, base, free_vram_bytes, n_ubatch, &out)
        && !try_expert_offload(shape, base, budget, &out)
        && !try_fewer_layers(shape, base, budget, &out)) {
        out.reason = "nothing fits, even with no layers offloaded";
        return out;
    }
    // The budget already has the compute allowance netted out, so this is
    // the real physical remainder, not an accounting one. Recomputed rather
    // than threaded back out of four call sites: `try_smaller_ubatch` moves
    // the budget, so the rung it chose is the one to price against.
    const uint64_t chosen = compute_allowance_bytes(
        base, out.n_ubatch > 0 ? out.n_ubatch : n_ubatch);
    const uint64_t effective =
        free_vram_bytes > chosen ? free_vram_bytes - chosen : 0;
    out.margin_bytes = effective > out.bytes ? effective - out.bytes : 0;
    return out;
}

}  // namespace entropic
