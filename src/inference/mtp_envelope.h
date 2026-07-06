// SPDX-License-Identifier: Apache-2.0
/**
 * @file mtp_envelope.h
 * @brief Pure envelope check for the MTP speculative path (gh#108).
 *
 * MTP (multi-token-prediction) decode is correct in the greedy (temperature=0),
 * unconstrained (no grammar), non-streaming envelope. Outside it, the engine
 * must FAIL LOUDLY so the consumer corrects the config — never silently fall
 * back to plain decode (which masks that MTP never ran). This predicate is a
 * pure function so the decision is CPU-unit-testable independent of any
 * GPU/model; the backend wiring that consults it is verified by model tests.
 *
 * gh#108 (v2.9.2): TOOLS are NOT a guard. MTP is lossless at temp=0, gemma4
 * tool-calling is parsed post-hoc (not sampler-grammar-constrained), so MTP +
 * staged tools produces the same tool call as plain decode. The earlier "tools"
 * guard was over-broad; the real gap was stop-handling, now fixed (MTP honors
 * effective_stop). This makes MTP reachable through the agent loop.
 *
 * gh#108 (v2.9.3): the flash_attn guard is dropped. It existed because the
 * gemma4-assistant head (GQA-2 + head_dim-512) hit an unsupported MMA
 * specialization in ggml_cuda_flash_attn_ext and GGML_ABORTed. Upstream
 * llama.cpp #25148 ("CUDA: fix Gemma E4B MTP FlashAttention", merged
 * 2026-06-30) restores that specialization; the extern/llama.cpp pin was
 * bumped past it, so MTP + flash_attn is now safe (and unblocks quantized KV,
 * which requires flash).
 *
 * @version 2.9.3
 */

#pragma once

#include <string>

namespace entropic {

/**
 * @brief Reason MTP cannot run for a request, or "" when the envelope is safe.
 *
 * @param temperature Effective sampling temperature (>0 ⇒ stochastic).
 * @param has_grammar True when a GBNF grammar constraint (params.grammar) is
 *        active — a sampler constraint to_common_sampling drops (NOTE: gemma4
 *        common_chat tool grammars are post-hoc parse, not this).
 * @param streaming True when a per-token callback is bound (streaming call).
 * @return Actionable message when MTP is unsupported for the request, else "".
 * @utility
 * @version 2.9.3
 */
inline std::string mtp_unsupported_reason(float temperature, bool has_grammar,
                                          bool streaming) {
    std::string r;
    if (temperature > 0.0f) {
        r = "MTP is lossless only at temperature=0 (the accept step is naive "
            "token-equality, not rejection sampling); set temperature=0 or "
            "disable speculative.mtp";
    } else if (has_grammar) {
        r = "MTP does not enforce grammar constraints; disable speculative.mtp "
            "for grammar-constrained tiers";
    } else if (streaming) {
        r = "MTP does not support streaming (the thinking-channel strip is a "
            "post-buffer operation); disable speculative.mtp for streaming calls";
    }
    return r;
}

}  // namespace entropic
