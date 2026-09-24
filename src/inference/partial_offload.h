// SPDX-License-Identifier: Apache-2.0
/**
 * @file partial_offload.h
 * @brief How many layers of an oversized GGUF fit the free VRAM, purely.
 *
 * A model larger than the card runs partially offloaded: some layers on the
 * GPU, the rest in system RAM. Choosing that split badly is how a "slow but
 * working" configuration becomes an OOM, and the direction is easy to invert
 * — every layer moved OFF the GPU ADDS to the host footprint, so more CPU
 * offload makes memory pressure worse, not better.
 *
 * The v2.7.0 accommodation hardcoded 15 layers from a measurement taken while
 * the desktop held ~1.8 GB of VRAM. It never adapted, so on a quiet card
 * reporting 10.2 GB free it still left ~8.2 GB of a 13.2 GB model in RAM.
 *
 * Pure and vendor-free so the arithmetic is CPU-unit-testable with no GPU and
 * no model — the same reason warm_keep_util.h and session_pool_util.h are.
 *
 * @version 2.12.0
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace entropic {

/// @brief VRAM held back for compute/graph buffers, which scale with ubatch
///        and model internals rather than with layer count. The v2.7.0 note
///        records a compute-buffer OOM at 20 layers when only ~2 GB was
///        spare, so this is deliberately generous.
constexpr uint64_t kPartialOffloadReserveBytes = 2048ull * 1024 * 1024;

/**
 * @brief Layers to place on the GPU for an oversized model.
 *
 * @param file_bytes Size of the GGUF on disk.
 * @param free_vram_bytes Free VRAM reported by the device right now.
 * @param n_layers Total layers. ESTIMATE when the caller does not know it:
 *                 the count is GGUF metadata this header will not read, and
 *                 the two models that reach here have 30 and 40. The default
 *                 is deliberately the LOW end, because overestimating
 *                 n_layers understates per-layer cost and computes MORE
 *                 layers than fit — the dangerous direction. Underestimating
 *                 merely leaves GPU capacity unused. Pass the real value
 *                 when it is known.
 * @return Layer count in [0, n_layers]; 0 when nothing fits.
 * @req REQ-INFER-019
 * @version 2.12.0
 */
inline int partial_gpu_layers_for(uint64_t file_bytes,
                                  uint64_t free_vram_bytes,
                                  int n_layers = 30) {
    if (file_bytes == 0 || n_layers <= 0) { return 0; }
    if (free_vram_bytes <= kPartialOffloadReserveBytes) { return 0; }
    const uint64_t budget = free_vram_bytes - kPartialOffloadReserveBytes;
    // Per-layer cost approximated from the file: exact enough to choose a
    // split, and it needs no GGUF metadata this header refuses to read.
    const uint64_t per_layer =
        file_bytes / static_cast<uint64_t>(n_layers);
    if (per_layer == 0) { return n_layers; }
    const auto fits = static_cast<int>(budget / per_layer);
    return std::clamp(fits, 0, n_layers);
}

/**
 * @brief Whether the host can hold a model's WARM load.
 *
 * The WARM state maps the ENTIRE GGUF into CPU RAM regardless of gpu_layers —
 * measured at 12952 MiB for a 13.6 GB file — and only the ACTIVE reload
 * honours the partial-offload split. So peak host usage is the whole file,
 * not the CPU-side remainder, which is why a 12 GB model is marginal on a
 * 31 GB box rather than comfortable.
 *
 * @param file_bytes Size of the GGUF on disk.
 * @param available_bytes Host MemAvailable right now.
 * The requirement is NOT file_bytes plus a constant. The engine loads WARM
 * (whole file in host RAM), then unloads and reloads for ACTIVE, and those
 * two residencies can overlap — so peak approaches TWICE the file size. A
 * first cut used file + 2 GB, did not fire at ~15 GB available for a 12.6 GB
 * model, and the load died anyway at buffer allocation. Two data points also
 * rule out a pure size rule: the DENSE 13.6 GB gemma-4-26B succeeds where
 * the smaller 12.6 GB hybrid Qwen fails, because the recurrent state is
 * additional.
 *
 * @param headroom_bytes Slack beyond the doubled residency for context,
 *                       compute buffers and the process itself.
 * @warning BEST-EFFORT, and known to miss cases. The caller's usual source
 *          for `available_bytes` is /proc/meminfo MemAvailable, which counts
 *          RECLAIMABLE PAGE CACHE — and after a model suite has run, the
 *          model files themselves ARE that cache. So it can report ~20 GB
 *          available while satisfying a 20 GB allocation would require
 *          evicting exactly the cache it counted, and the load dies anyway.
 *          Observed doing precisely that: this predicate admitted a 12.6 GB
 *          hybrid model at a reported ~15-20 GB available and the load was
 *          killed at buffer allocation.
 *
 *          It is kept because it costs nothing and does fire on a genuinely
 *          loaded machine. It is NOT a guarantee, and a green suite does not
 *          mean the gate protected anything. A reliable check would need a
 *          pressure-based metric (PSI) rather than a free-memory estimate.
 *
 * @return true when the load should be attempted. An unknown availability
 *         (0) returns true: refusing to run because we could not measure is
 *         worse than trying.
 * @req REQ-INFER-019
 * @version 2.12.0
 */
inline bool host_can_hold_warm_load(
    uint64_t file_bytes, uint64_t available_bytes,
    uint64_t headroom_bytes = 1024ull * 1024 * 1024) {
    if (available_bytes == 0) { return true; }
    // WARM and ACTIVE residencies can overlap across the reload boundary.
    const uint64_t peak = file_bytes + (file_bytes / 2);
    return available_bytes >= peak + headroom_bytes;
}

/// @brief Size above which a model counts as "large" for the waiver below.
///        The two models that have ever needed the allowance are 12.6 GB and
///        13.6 GB; every other model in the suite is under 6 GB and must stay
///        un-waivable, so a mis-set allowance cannot silently blank the run.
constexpr uint64_t kLargeModelThresholdBytes = 10ull * 1024 * 1024 * 1024;

/**
 * @brief Whether an operator has explicitly waived one LARGE model's test.
 *
 * The MemAvailable predicate above is best-effort and documented as
 * unreliable, which makes it a poor basis for a RELEASE record — the same
 * commit skips or does not depending on page-cache state at that instant.
 * `ENTROPIC_SKIP_LARGE_MODEL_TESTS=1` records the decision explicitly, so
 * results.json says the same thing on every run.
 *
 * The size floor is HERE rather than at the call sites on purpose. A waiver
 * that applies to every model is not an allowance, it is a mute button: set
 * it once and a whole green suite means nothing. Both model-test load paths
 * consult this one predicate, so neither can drift into that shape.
 *
 * Deliberately opt-IN: unset, a capable host still attempts the load. This
 * records an operator's decision; it does not make one.
 *
 * @param file_bytes Size of the GGUF on disk. A model at or below
 *                   kLargeModelThresholdBytes is never waivable and returns
 *                   false however the environment is set.
 * @return true only when the allowance is set AND the model is large.
 * @req REQ-INFER-019
 * @version 2.12.0
 */
inline bool large_model_tests_waived(uint64_t file_bytes) {
    if (file_bytes <= kLargeModelThresholdBytes) { return false; }
    const char* v = std::getenv("ENTROPIC_SKIP_LARGE_MODEL_TESTS");
    return v != nullptr && v[0] == '1';
}

/// @brief Sentinel for an unlimited RLIMIT_MEMLOCK (gh#148).
constexpr uint64_t kMemlockUnlimited = UINT64_MAX;

/**
 * @brief Whether an explicit `use_mlock` would be fatal for this model.
 *
 * Locking pages that exceed `RLIMIT_MEMLOCK` does not merely fail to
 * optimise: llama.cpp locks what it can, up to the limit, and the pages it
 * DID lock cannot be reclaimed under pressure. That is what turned partial
 * offload of a 13 GB model from slow into OOM-killed (v2.12.0). The harness
 * discovered this and flipped `use_mlock` off behind the operator's back;
 * the engine says so instead.
 *
 * **Floor-gated to LARGE models on purpose.** `use_mlock` DEFAULTS to true,
 * so an un-floored rule would refuse the ordinary case of a CPU-resident
 * 4 GB model on a box with the usual 8 MB limit — a configuration that
 * works today (llama.cpp warns and continues) and that nobody asked to
 * change. Only a model too large for the card in the first place reaches
 * the pressure this predicate is about.
 *
 * @param use_mlock The configured value.
 * @param file_bytes Size of the GGUF on disk.
 * @param gpu_layers Configured offload; -1 (all) is exempt because the host
 *                   side is then transient rather than resident.
 * @param memlock_limit_bytes RLIMIT_MEMLOCK, or kMemlockUnlimited.
 * @return true when the configuration should be REFUSED.
 * @req REQ-INFER-019
 * @version 2.13.0
 */
inline bool mlock_refused(bool use_mlock, uint64_t file_bytes,
                          int gpu_layers, uint64_t memlock_limit_bytes,
                          uint64_t host_expert_bytes = 0) {
    // v2.13.1: `gpu_layers < 0` exempts because the host side is then
    // TRANSIENT — nothing stays in host RAM to be pinned. An expert split
    // breaks that premise: `cpu_moe_layers` deliberately leaves expert
    // weights host-RESIDENT while every layer reports as offloaded, so the
    // sentinel alone would exempt exactly the configuration the rule exists
    // to catch. Reachable since v2.13.1, when `gpu_layers: auto` began
    // deriving expert splits and emitting -1 alongside them.
    const bool host_side_is_transient =
        gpu_layers < 0 && host_expert_bytes == 0;
    const bool exempt = !use_mlock
        || host_side_is_transient
        || file_bytes <= kLargeModelThresholdBytes      // never refuse small
        || memlock_limit_bytes == kMemlockUnlimited;
    if (exempt) { return false; }
    // Under an expert split it is the HOST-resident expert bytes that get
    // pinned, not the whole file; pricing the file would refuse splits that
    // comfortably fit the limit.
    const uint64_t pinned =
        host_expert_bytes > 0 ? host_expert_bytes : file_bytes;
    return pinned > memlock_limit_bytes;
}

/**
 * @brief Whether a requested GPU offload provably cannot fit free VRAM.
 *
 * Fires only on the case the measurement settles WITHOUT knowing the
 * model's layer count: free VRAM at or below the compute-buffer reserve
 * means no positive `gpu_layers` can work, however the file divides into
 * layers. That is the "died at buffer allocation" report.
 *
 * It deliberately does NOT try to judge a specific partial split. Doing so
 * needs the layer count, which is GGUF metadata the admission gate does not
 * read, and guessing it wrong refuses configurations that work — the worse
 * failure, and the same reason `estimate_footprint_bytes` reports a
 * partially offloaded tier as unpriceable rather than inventing a number.
 *
 * Floor-gated to LARGE models for the same reason as `mlock_refused`: a
 * 500 MB model on a card with 1.5 GB free runs fine, and the 2 GiB reserve
 * is sized for 13 GB models.
 *
 * @param file_bytes Size of the GGUF on disk.
 * @param gpu_layers Configured offload (0 = CPU-only, exempt).
 * @param free_vram_bytes Free VRAM; 0 means unmeasured and never refuses.
 * @return true when the configuration should be REFUSED.
 * @req REQ-INFER-019
 * @version 2.13.0
 */
inline bool gpu_offload_refused(uint64_t file_bytes, int gpu_layers,
                                uint64_t free_vram_bytes) {
    const bool exempt = gpu_layers == 0
        || free_vram_bytes == 0
        || file_bytes <= kLargeModelThresholdBytes;
    return !exempt && free_vram_bytes <= kPartialOffloadReserveBytes;
}

}  // namespace entropic
