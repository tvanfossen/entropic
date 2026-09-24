// SPDX-License-Identifier: Apache-2.0
/**
 * @file expert_offload.h
 * @brief EXPERIMENTAL (v2.13.0): put a MoE layer's routed-expert tensors on
 *        the host while its attention, KV and dense weights stay on the card.
 *
 * @par Why this exists
 * `gpu_layers` is role-blind. A layer is either wholly on the device or
 * wholly on the host, so a model that does not fit spends VRAM on rarely
 * touched expert weights and exiles attention — **and its KV** — to system
 * RAM. Measured at v2.13.0 (gh#153, decision #42): Gemma 4 26B-A4B QAT
 * (14.25 GB) on an 11 GiB GTX 1080 Ti resolves `gpu_layers: auto` to 18 of
 * 31 layers and runs at 18.7 tok/s, and MTP — worth +42.8 % on the fully
 * resident E4B — is worth nothing there (−0.92 % / +0.82 % across two runs
 * against a same-config floor of 0.5–1.0 %). Decision #42 item (iii) records
 * that the split "moves WHOLE layers, attention included, and says nothing
 * about an experts-only split". This is the knob that lets someone measure
 * the other split.
 *
 * Published practice is consistent about the direction — KTransformers,
 * Fiddler (ICLR'25) and mixtral-offloading all keep attention, KV and the
 * shared/dense path resident and push only routed-expert FFN weights to the
 * host. llama.cpp exposes exactly that through
 * `llama_model_params::tensor_buft_overrides` (`-ncmoe` / `--n-cpu-moe`),
 * which is the mechanism this header mirrors rather than reinvents.
 *
 * @par Why this header is pure
 * Same reason as `partial_offload.h` and `session_pool_util.h`: the rule is
 * string construction plus a handful of integer refusals, and it must be
 * assertable by a CPU unit test with no GPU, no GGUF and no llama.cpp. The
 * binding to `llama_model_tensor_buft_override` lives next door in
 * `expert_offload_buft.h`, which is the only part that needs the vendor
 * headers — and which `validate.cpp` therefore does not have to include.
 *
 * @warning PROTOTYPE, default off. As of v2.13.1 `gpu_layers: auto` DOES
 *          model expert placement — it reads the real expert-tensor bytes
 *          from GGUF metadata and prefers an expert split over dropping a
 *          layer. The two remain mutually exclusive, but for the opposite
 *          reason: auto derives the split itself, so setting both asks two
 *          things to decide one placement.
 *
 * @version 2.13.0
 */

#pragma once

#include <entropic/types/config.h>

#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Tensor-name pattern tail identifying a layer's ROUTED-expert weights.
 *
 * Copied verbatim from the vendored llama.cpp at this pin —
 * `LLM_FFN_EXPS_REGEX` in `extern/llama.cpp/common/common.h`, the same
 * constant `--n-cpu-moe` builds its overrides from. It is duplicated rather
 * than included so this header stays vendor-free; `expert_offload_test.cpp`
 * includes `<common.h>` and asserts the two are equal, so a pin bump that
 * changes upstream's pattern fails a CPU unit test instead of silently
 * offloading nothing.
 *
 * llama.cpp matches it with `std::regex_search` against the full tensor name
 * (`llama-model-loader.cpp`), so it is a SUBSTRING match. For the `gemma4`
 * MoE arch at this pin that covers `blk.N.ffn_gate_exps.weight`,
 * `blk.N.ffn_up_exps.weight`, `blk.N.ffn_down_exps.weight`, the fused
 * `blk.N.ffn_gate_up_exps.weight`, and their per-expert `.scale` siblings —
 * and nothing else. The router (`ffn_gate_inp`), the shared/dense FFN
 * (`ffn_gate`/`ffn_up`/`ffn_down`), every `*_shexp`, attention, norms and
 * embeddings do NOT match, which is precisely the point: they stay wherever
 * `gpu_layers` put them.
 * @version 2.13.0
 */
constexpr const char* kExpertTensorRegex =
    "\\.ffn_(up|down|gate|gate_up)_(ch|)exps";

/**
 * @brief The override pattern for ONE layer's expert tensors.
 *
 * Mirrors upstream's `llm_ffn_block_regex(idx, LLM_FFN_EXPS_REGEX)`:
 * `blk\.<idx>` followed by the expert tail. The `blk\.` prefix is anchored
 * by the literal `\.` that opens the tail, so the pattern for layer 1 does
 * NOT match `blk.10.ffn_up_exps.weight` — after `blk.1` the next character
 * must be a dot.
 *
 * @param layer Zero-based layer index.
 * @return ECMAScript regex matching only that layer's routed-expert tensors.
 * @req REQ-INFER-027
 * @version 2.13.0
 */
inline std::string expert_block_pattern(int layer) {
    return "blk\\." + std::to_string(layer) + kExpertTensorRegex;
}

/**
 * @brief Patterns for the FIRST `cpu_moe_layers` layers, in layer order.
 *
 * The first N layers, not an arbitrary set — that is upstream's semantic for
 * `--n-cpu-moe` and there is no reason for a prototype to invent a second
 * one. Zero or negative yields an empty list, which is what makes the absent
 * config key produce no overrides at all.
 *
 * @warning "The first N layers" is not the same as "N layers' worth of
 *          experts". `gemma4` decides per layer whether it has experts at all
 *          (`has_expert = ffn_gate_inp != nullptr` in `models/gemma4.cpp`),
 *          so on a model with dense early blocks some of the first N patterns
 *          match nothing and the VRAM actually freed is less than N layers'
 *          worth. That is upstream's behaviour too, and it matters when
 *          READING a measurement: compare `load_tensors:` buffer sizes, not
 *          the configured count.
 *
 * @param cpu_moe_layers Number of layers whose experts go to the host.
 * @return One pattern per layer; empty when the feature is off.
 * @req REQ-INFER-027
 * @version 2.13.0
 */
inline std::vector<std::string> expert_offload_patterns(int cpu_moe_layers) {
    std::vector<std::string> patterns;
    if (cpu_moe_layers <= 0) { return patterns; }
    patterns.reserve(static_cast<std::size_t>(cpu_moe_layers));
    for (int i = 0; i < cpu_moe_layers; ++i) {
        patterns.push_back(expert_block_pattern(i));
    }
    return patterns;
}

/**
 * @brief Configuration-time refusal for a combination that cannot mean what
 *        it says.
 *
 * Checked before any file is opened, so it costs nothing and fires at
 * `validate_config` with the tier named. Three cases, none of them clamped:
 *
 *  - **Negative N.** There is deliberately no "all layers" sentinel. `-1`
 *    means "every layer" for `gpu_layers` because llama.cpp defines it that
 *    way; nothing defines it here, and inventing it would need a layer count
 *    the config layer does not have.
 *  - **N > 0 with `gpu_layers: 0`.** The whole model is already host-side;
 *    there is nothing on the card to move off it. Accepting this would make
 *    the knob a no-op, and a no-op that an operator explicitly asked for is
 *    exactly the fail-open this codebase refuses.
 *  - **N > 0 with `gpu_layers: auto`.** Since v2.13.1 auto derives the
 *    expert split itself, from the model's real expert-tensor bytes. Setting
 *    both asks two things to decide one placement; drop this knob and let
 *    auto choose, or state `gpu_layers` explicitly and keep your own split.
 *
 * @param cfg Tier config as parsed.
 * @return Empty when acceptable; an operator-actionable reason otherwise.
 * @req REQ-CFG-006
 * @req REQ-INFER-027
 * @version 2.13.1
 */
inline std::string expert_offload_conflict_reason(const ModelConfig& cfg) {
    const int n = cfg.cpu_moe_layers;
    std::string reason;
    if (n < 0) {
        reason = "cpu_moe_layers must be >= 0 (0 = off), got "
               + std::to_string(n)
               + ". There is no 'all layers' sentinel — name the layer "
                 "count you want host-side.";
    } else if (n > 0 && cfg.gpu_layers == 0) {
        reason = "cpu_moe_layers=" + std::to_string(n)
               + " with gpu_layers: 0 — the whole model is already on the "
                 "host, so there are no expert tensors on the card to move "
                 "off it. Raise gpu_layers, or drop cpu_moe_layers.";
    } else if (n > 0 && cfg.gpu_layers_auto) {
        // v2.13.1: auto now derives the expert split ITSELF, from the GGUF's
        // real expert-tensor bytes, and prefers moving experts host-side
        // over dropping a layer. So this is no longer "auto cannot model
        // this" — it is "you have asked for both, and only one can win".
        reason = "cpu_moe_layers=" + std::to_string(n)
               + " with gpu_layers: auto — auto derives the expert split "
                 "itself from the model's shape, so setting both asks two "
                 "things to decide one placement. Drop cpu_moe_layers and "
                 "let auto choose, or set gpu_layers to an explicit count "
                 "and keep your own split.";
    }
    return reason;
}

/**
 * @brief Post-load refusal, against the numbers only the loaded model knows.
 *
 * The two impossible combinations that CANNOT be judged from config alone —
 * both need GGUF metadata (`general.architecture`, `<arch>.expert_count`,
 * the block count), and entropic reads no GGUF metadata before loading. So
 * this runs immediately after `llama_model_load_from_file` and FAILS the
 * load; the caller frees the model. A prototype pays one wasted load to
 * refuse rather than run a configuration that quietly did nothing:
 *
 *  - **Not a MoE model.** Every pattern would match zero tensors and the
 *    operator would measure "expert offload changes nothing" and conclude
 *    something false about the technique.
 *  - **N beyond the block count.** The surplus patterns are harmless to
 *    llama.cpp, but the request is not satisfiable as stated, and silently
 *    treating it as "all layers" is the clamp this codebase does not make.
 *
 * @param cpu_moe_layers Configured value; <= 0 accepts unconditionally.
 * @param n_layer Block count reported by `llama_model_n_layer`.
 * @param n_expert `<arch>.expert_count` from GGUF metadata; 0 when absent.
 * @return Empty when acceptable; an operator-actionable reason otherwise.
 * @req REQ-INFER-027
 * @version 2.13.1
 */
inline std::string expert_offload_load_refusal(int cpu_moe_layers,
                                               int n_layer, int n_expert) {
    std::string reason;
    if (cpu_moe_layers <= 0) { return reason; }
    if (n_expert <= 0) {
        reason = "cpu_moe_layers=" + std::to_string(cpu_moe_layers)
               + " was requested, but this model declares no experts "
                 "(<arch>.expert_count is absent or 0). Expert-tensor "
                 "offload has nothing to place; use gpu_layers to move "
                 "whole layers on a dense model.";
    } else if (cpu_moe_layers > n_layer) {
        reason = "cpu_moe_layers=" + std::to_string(cpu_moe_layers)
               + " exceeds the model's " + std::to_string(n_layer)
               + " layers. Lower it to at most " + std::to_string(n_layer)
               + "; it is not clamped, because a count that cannot be "
                 "honoured is a configuration error, not a preference.";
    }
    return reason;
}

}  // namespace entropic
