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
 * gh#108 (v2.9.4): the temperature>0 guard is dropped. It assumed the accept
 * step needed rejection sampling to be lossless at temperature>0 — but
 * common_speculative_impl_draft_mtp::draft() (extern/llama.cpp/common/
 * speculative.cpp) always proposes cur_p->data[0].id, the argmax of the
 * (top-k=10-filtered) draft distribution, and never the stochastically
 * selected candidate — this holds regardless of backend_sampling, since
 * draft() never reads cur_p->selected. The draft proposal is therefore a
 * deterministic point mass at every temperature. For a point-mass proposal
 * q(m)=1, the general Leviathan/Chen rejection-sampling accept rule
 * (accept-prob min(1, p_target(t)/p_draft(t)), resample residual on reject)
 * collapses algebraically to exactly what entropic's accept step already
 * does: draw one real sample s ~ p_target (common_sampler_sample_and_accept_n,
 * extern/llama.cpp/common/sampling.cpp, draws through the target's full real
 * filter chain), accept iff s == draft[i], else emit s as the correction.
 * This is lossless at any temperature, not just temperature=0 — provided the
 * draft proposal stays a point mass. If a future extern/llama.cpp pin bump
 * changes draft() to honor cur_p->selected instead of data[0], this proof no
 * longer holds and the guard must be reinstated; the statistical distribution
 * test in test_gh108_mtp_guards.cpp is the regression tripwire for that.
 *
 * @version 2.9.4
 */

#pragma once

#include <string>

namespace entropic {

/**
 * @brief Reason MTP cannot run for a request, or "" when the envelope is safe.
 *
 * temperature is no longer a guard (gh#108, v2.9.4): the draft proposal is a
 * deterministic point mass (see the file-level doc comment), so the existing
 * exact-match accept step is lossless at any temperature, not just 0.
 *
 * @param temperature Effective sampling temperature (unused; kept in the
 *        signature so call sites don't need to change if a future
 *        extern/llama.cpp pin bump reinstates the need for a temperature
 *        guard — see the v2.9.4 file-level doc comment).
 * @param has_grammar True when a GBNF grammar constraint (params.grammar) is
 *        active — a sampler constraint to_common_sampling drops (NOTE: gemma4
 *        common_chat tool grammars are post-hoc parse, not this).
 * @param streaming True when a per-token callback is bound (streaming call).
 * @return Actionable message when MTP is unsupported for the request, else "".
 * @utility
 * @version 2.9.4
 */
inline std::string mtp_unsupported_reason(float temperature, bool has_grammar,
                                          bool streaming) {
    (void)temperature;
    std::string r;
    if (has_grammar) {
        r = "MTP does not enforce grammar constraints; disable speculative.mtp "
            "for grammar-constrained tiers";
    } else if (streaming) {
        r = "MTP does not support streaming (the thinking-channel strip is a "
            "post-buffer operation); disable speculative.mtp for streaming calls";
    }
    return r;
}

}  // namespace entropic
