// SPDX-License-Identifier: Apache-2.0
/**
 * @file vram_footprint.h
 * @brief gh#142: a VRAM footprint estimate honest enough to refuse a load on.
 *
 * @par Why this exists
 * The v2.2.4 admission gate (REQ-INFER-019) compared a tier's estimated
 * footprint against a VRAM budget and refused with
 * `ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE`. It was never reached in practice: the
 * budget came only from `ENTROPIC_VRAM_BUDGET_BYTES`, so on any deployment that
 * did not set that variable the budget was 0, the gate was disabled, and a
 * failed allocation aborted the host process inside llama.cpp
 * (`ggml-backend.cpp:179: GGML_ASSERT(buffer)`) with no diagnostic.
 *
 * Switching the gate on required fixing the estimate first, because the old one
 * was wrong in three ways that all bias toward REFUSING configurations that
 * work:
 *   - it counted the entire weights file no matter how many layers were
 *     actually offloaded, so a partially offloaded model was priced as if fully
 *     resident. Qwen3.6-35B-A3B IQ3_XXS (~13 GB) runs at `gpu_layers=15` on an
 *     11 GB card; a gate built on that estimate would refuse it.
 *   - it priced KV at a flat 16 KiB/token regardless of `cache_type`, roughly
 *     4x over-counting a q4_0 cache.
 *   - it ignored the vision projector, which is the allocation that actually
 *     failed in the gh#142 abort.
 *
 * @par What this does NOT count — compute buffers
 * llama.cpp reserves graph/activation scratch per context at load time, sized by
 * ubatch and model internals rather than by context length. Measured on this
 * repo's own benchmark: **1222 MiB** for a gemma-4 E4B MTP head's context, at a
 * 512-token ubatch. That is more than twice the default `vram_reserve_mb` of
 * 512, and a speculative configuration pays it TWICE because it holds two
 * contexts.
 *
 * It is not counted here because it cannot be derived from anything this header
 * is willing to read — it needs the model's hidden size and layer count, i.e.
 * GGUF metadata. `vram_reserve_mb` is the knob that must cover it, and callers
 * running near the edge should raise it rather than trust the default.
 *
 * The consequence is honest and worth stating plainly: **this estimate can admit
 * a configuration that then fails to load.** It is designed to prevent the
 * catastrophic case (an abort that kills the host process) and to produce an
 * actionable recommendation — not to guarantee a load succeeds. A load that
 * fails after admission surfaces as a typed error, which is the outcome gh#142
 * asked for.
 *
 * @par The rule this file follows
 * An estimate that cannot be bounded is reported as UNKNOWN rather than guessed.
 * An unknown estimate leaves the gate open — the engine declines to refuse what
 * it cannot price. Refusing a working configuration is a worse failure than
 * missing one that would have failed, because the operator has no way to tell a
 * false refusal from a real one.
 *
 * Kept pure and free of vendor types so every case is CPU-unit-testable; the
 * device query that supplies `available` lives in device_memory.h.
 *
 * @version 2.11.0
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace entropic {

/// @brief KV bytes per token at f16, the rate the v2.2.4 estimate assumed.
constexpr uint64_t kBaseKvPerTokenF16 = 16ull * 1024ull;

/// @brief `gpu_layers` at or above this means "every layer" (llama.cpp's 99).
constexpr int kAllLayersSentinel = 99;

/// @brief Where a tier's weights land, as far as the estimate can tell.
enum class Offload {
    none,             ///< gpu_layers == 0: weights cost no VRAM.
    full,             ///< -1 or >= 99: the whole file is resident.
    partial_unknown,  ///< A layer count we cannot price without GGUF metadata.
};

/// @brief Everything the estimate needs, with no orchestrator or filesystem.
struct FootprintInputs {
    uint64_t weights_bytes = 0;   ///< Size of the tier's GGUF on disk.
    uint64_t mmproj_bytes = 0;    ///< Size of the vision projector, 0 if none.
    int gpu_layers = -1;          ///< Tier's requested offload (-1 = all).
    int context_length = 0;       ///< Tier's requested context window.
    std::string cache_type_k = "f16";  ///< KV key cache quantization.
    std::string cache_type_v = "f16";  ///< KV value cache quantization.
    int vram_reserve_mb = 0;      ///< Configured headroom to leave free.

    /// @brief Resident conversation sessions this tier keeps KV for (gh#144).
    ///
    /// The KV term multiplies by this. `context_length` is PER SESSION, so a
    /// pool of 3 at 32768 allocates 98304 cells. Without this field the gate
    /// under-counts a pool by exactly N and can admit a configuration that
    /// then aborts the host process inside llama.cpp — the precise failure
    /// gh#142 built this gate to prevent.
    /// @version 2.12.0
    int max_sessions = 1;

    /// @brief Draft / MTP head bytes, resident alongside the target.
    ///
    /// v2.13.1: priced nowhere before. A target-owned MTP head is small
    /// (225 MiB for `mtp_a4b`) but it is loaded onto the same card, and on
    /// an 11 GiB device against a 9.3 GiB trunk it is the difference
    /// between fitting and not.
    /// @version 2.13.1
    uint64_t draft_bytes = 0;

    /// @name GGUF-derived shape (v2.13.1)
    ///
    /// Zero means "not read", and the estimator then behaves exactly as it
    /// did before — a partial offload stays unpriceable. Supplied, they let
    /// a partial offload be priced EXACTLY, which is what previously forced
    /// `partial_unknown` and left the budget gate open (gh#142).
    /// @{
    int block_count = 0;           ///< Real layer count from `<arch>.block_count`.
    uint64_t block_bytes = 0;      ///< Total bytes of per-block tensors.
    uint64_t non_block_bytes = 0;  ///< Embeddings/output/final norms.

    /// @brief Expert bytes deliberately kept host-side by `cpu_moe_layers`.
    ///
    /// Subtracted from the resident weight term: this is what an expert
    /// split actually frees on the card, summed from the tensors that match
    /// llama.cpp's expert pattern rather than guessed as a fraction.
    uint64_t host_expert_bytes = 0;
    /// @}
};

/// @brief An estimate, or an explicit admission that there isn't one.
struct FootprintEstimate {
    bool known = false;      ///< False means "do not gate on this".
    uint64_t bytes = 0;      ///< Estimated VRAM bytes; 0 when not known.
    const char* reason = ""; ///< Why it is unknown, for the log.
};

/**
 * @brief Cost of a KV cache type relative to f16.
 *
 * ggml packs 32 elements per quantized block, so the per-element cost is the
 * block size over 32, expressed here against f16's 2 bytes/element. An
 * unrecognised type is priced as f16 so the estimate never silently
 * under-counts and lets through a load that will not fit.
 *
 * @param type Cache type string from the tier config ("f16", "q4_0", ...).
 * @return Multiplier against the f16 rate.
 * @req REQ-INFER-019
 * @version 2.11.0
 */
inline double kv_scale_for_cache_type(const std::string& type) {
    static const std::pair<const char*, double> kScales[] = {
        {"f32",  2.0},      // 4 B/elem
        {"f16",  1.0},      // 2 B/elem  — the baseline
        {"bf16", 1.0},      // 2 B/elem
        {"q8_0", 0.53125},  // 34 B / 32 elem = 1.0625 B/elem
        {"q5_1", 0.375},    // 24 B / 32 elem = 0.75   B/elem
        {"q5_0", 0.34375},  // 22 B / 32 elem = 0.6875 B/elem
        {"q4_1", 0.3125},   // 20 B / 32 elem = 0.625  B/elem
        {"q4_0", 0.28125},  // 18 B / 32 elem = 0.5625 B/elem
    };
    double scale = 1.0;
    for (const auto& entry : kScales) {
        if (type == entry.first) {
            scale = entry.second;
            break;
        }
    }
    return scale;
}

/**
 * @brief Classify a tier's offload request into a priceable placement.
 *
 * A specific positive layer count below the sentinel cannot be turned into
 * bytes without knowing the model's total layer count, which needs GGUF
 * metadata this estimate deliberately does not read.
 *
 * @param gpu_layers Tier's configured offload layer count.
 * @return Which placement the estimate can assume.
 * @req REQ-INFER-019
 * @version 2.11.0
 */
inline Offload classify_offload(int gpu_layers) {
    Offload placement = Offload::partial_unknown;
    if (gpu_layers < 0 || gpu_layers >= kAllLayersSentinel) {
        placement = Offload::full;
    } else if (gpu_layers == 0) {
        placement = Offload::none;
    }
    return placement;
}

/**
 * @brief Per-token KV cost in bytes for this tier's cache configuration.
 *
 * Averages the key and value scales, which may differ — a tier may quantize one
 * and not the other.
 *
 * @param in Tier footprint inputs.
 * @return Bytes of KV cache per token of context.
 * @req REQ-INFER-019
 * @version 2.11.0
 */
inline double kv_bytes_per_token(const FootprintInputs& in) {
    const double avg = (kv_scale_for_cache_type(in.cache_type_k)
                        + kv_scale_for_cache_type(in.cache_type_v)) / 2.0;
    return static_cast<double>(kBaseKvPerTokenF16) * avg;
}

/**
 * @brief Estimate the VRAM a tier will occupy, or report that it cannot.
 *
 * @param in Tier footprint inputs.
 * @return The estimate, with `known == false` when the placement is unpriceable.
 * @req REQ-INFER-019
 * @version 2.13.1
 */
inline FootprintEstimate estimate_vram_footprint(const FootprintInputs& in) {
    FootprintEstimate est;
    const Offload placement = classify_offload(in.gpu_layers);
    // v2.13.1: a partial offload IS priceable once the GGUF's shape has been
    // read. Without it we still decline, because guessing here refuses
    // configurations that demonstrably work (gh#142) — but the shape is now
    // available from a metadata-only read, so declining is the fallback
    // rather than the rule.
    const bool have_shape = in.block_count > 0 && in.block_bytes > 0;
    if (placement == Offload::partial_unknown && !have_shape) {
        est.reason = "partial offload: layer count is not knowable without "
                     "GGUF metadata, so the budget gate stays open";
        return est;
    }
    uint64_t resident = 0;
    if (placement == Offload::full) {
        // The projector follows the weights: offload none and it lives in
        // host RAM.
        resident = in.weights_bytes + in.mmproj_bytes;
    } else if (placement == Offload::partial_unknown) {
        // Per-block cost from the block tensors ALONE. Dividing the whole
        // file by the layer count folds in embeddings and the output head,
        // which do not move with `gpu_layers`, and so overstates every
        // layer.
        const auto per_block =
            in.block_bytes / static_cast<uint64_t>(in.block_count);
        const auto offloaded = static_cast<uint64_t>(in.gpu_layers);
        resident = per_block * offloaded;
    }
    // Experts held host-side by `cpu_moe_layers` are not on the card.
    resident -= std::min(resident, in.host_expert_bytes);
    // The draft head is resident whenever speculative decode is configured,
    // at any placement.
    resident += in.draft_bytes;
    const int ctx = in.context_length > 0 ? in.context_length : 0;
    const int sessions = in.max_sessions > 0 ? in.max_sessions : 1;
    // gh#144 (v2.12.0): context_length is per session; the pool allocates
    // ctx * sessions cells. Deliberately an over-count for an iSWA model
    // like Gemma-4: kBaseKvPerTokenF16 is a flat rate and does not model
    // sliding-window layers (5:1 SWA at window 512 under swa_full=false),
    // so the real figure is well below this. That is the safe direction
    // per this header's own rule — never silently under-count.
    const uint64_t kv = static_cast<uint64_t>(
        static_cast<double>(ctx) * static_cast<double>(sessions)
        * kv_bytes_per_token(in));
    est.known = true;
    est.bytes = resident + kv
        + static_cast<uint64_t>(in.vram_reserve_mb) * 1024ull * 1024ull;
    return est;
}

/**
 * @brief Largest context length that fits, for the "won't fit" recommendation.
 *
 * Returns a value the caller can actually act on rather than a bare refusal.
 * Never inflates a request that already fits, and returns 0 when no context
 * fits at all (the weights alone exceed the card) or when the placement is
 * unpriceable — in both cases there is no honest number to offer.
 *
 * @param in Tier footprint inputs, carrying the requested context length.
 * @param available_bytes VRAM actually available on the device.
 * @return A context length in tokens, rounded down to a 512 multiple, or 0.
 * @req REQ-INFER-019
 * @version 2.12.0
 */
inline int recommend_context_length(const FootprintInputs& in,
                                    uint64_t available_bytes) {
    FootprintInputs probe = in;
    probe.context_length = 0;
    const FootprintEstimate fixed = estimate_vram_footprint(probe);
    if (!fixed.known || fixed.bytes >= available_bytes) {
        return 0;
    }
    // gh#144 (v2.12.0): divide the budget across the pool, or the
    // recommendation is a PER-SESSION window N times too large — handing an
    // operator a config that aborts, from the very function whose job is to
    // offer one that works.
    const int sessions = in.max_sessions > 0 ? in.max_sessions : 1;
    const double per_token =
        kv_bytes_per_token(in) * static_cast<double>(sessions);
    const uint64_t kv_budget = available_bytes - fixed.bytes;
    const uint64_t fits = static_cast<uint64_t>(
        static_cast<double>(kv_budget) / per_token);
    const uint64_t requested = in.context_length > 0
        ? static_cast<uint64_t>(in.context_length) : 0ull;
    uint64_t chosen = fits < requested ? fits : requested;
    chosen -= chosen % 512ull;
    return static_cast<int>(chosen);
}

}  // namespace entropic
