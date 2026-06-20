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
 * @version 2.9.2
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
 * @param flash_attn True when flash attention is enabled (config.flash_attn).
 * @return Actionable message when MTP is unsupported for the request, else "".
 * @utility
 * @version 2.9.2
 */
inline std::string mtp_unsupported_reason(float temperature, bool has_grammar,
                                          bool streaming, bool flash_attn) {
    std::string r;
    if (temperature > 0.0f) {
        r = "MTP is lossless only at temperature=0 (the accept step is naive "
            "token-equality, not rejection sampling); set temperature=0 or "
            "disable speculative.mtp";
    } else if (has_grammar) {
        r = "MTP does not enforce grammar constraints; disable speculative.mtp "
            "for grammar-constrained tiers";
    } else if (flash_attn) {
        // gh#108: the gemma4-assistant head is GQA-2 + head_dim-512, which the
        // flash MMA kernel does not cover at this llama.cpp pin → GGML_ABORT in
        // ggml_cuda_flash_attn_ext. Fail loud (don't silently override flash,
        // and don't crash). The no-flash path is validated + fast (~2.3x).
        r = "MTP aborts with flash attention on this llama.cpp pin (the "
            "gemma4-assistant head is GQA-2 + head_dim-512, unsupported by the "
            "flash kernel); set flash_attn=false for MTP tiers";
    } else if (streaming) {
        r = "MTP does not support streaming (the thinking-channel strip is a "
            "post-buffer operation); disable speculative.mtp for streaming calls";
    }
    return r;
}

}  // namespace entropic
