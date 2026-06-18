// SPDX-License-Identifier: Apache-2.0
/**
 * @file mtp_envelope.h
 * @brief Pure envelope check for the MTP speculative path (gh#108).
 *
 * MTP (multi-token-prediction) decode is correct ONLY in a narrow envelope:
 * greedy (temperature=0), unconstrained (no grammar), no staged tools, and
 * non-streaming. Outside it, the engine must FAIL LOUDLY so the consumer
 * corrects the config — never silently fall back to plain decode (which masks
 * that MTP never ran). This predicate is a pure function so the decision is
 * CPU-unit-testable independent of any GPU/model; the backend wiring that
 * consults it is verified by model tests.
 *
 * @version 2.9.1
 */

#pragma once

#include <string>

namespace entropic {

/**
 * @brief Reason MTP cannot run for a request, or "" when the envelope is safe.
 *
 * @param temperature Effective sampling temperature (>0 ⇒ stochastic).
 * @param has_grammar True when a GBNF grammar constraint is active.
 * @param has_tools True when tool definitions are staged for this render.
 * @param streaming True when a per-token callback is bound (streaming call).
 * @return Actionable message when MTP is unsupported for the request, else "".
 * @utility
 * @version 2.9.1
 */
inline std::string mtp_unsupported_reason(float temperature, bool has_grammar,
                                          bool has_tools, bool streaming) {
    std::string r;
    if (temperature > 0.0f) {
        r = "MTP is lossless only at temperature=0 (the accept step is naive "
            "token-equality, not rejection sampling); set temperature=0 or "
            "disable speculative.mtp";
    } else if (has_grammar) {
        r = "MTP does not enforce grammar constraints; disable speculative.mtp "
            "for grammar-constrained tiers";
    } else if (has_tools) {
        r = "MTP does not support tool-call generation (sequential-tool stop + "
            "parse snapshot are bypassed); disable speculative.mtp for tooled "
            "tiers";
    } else if (streaming) {
        r = "MTP does not support streaming (the thinking-channel strip is a "
            "post-buffer operation); disable speculative.mtp for streaming calls";
    }
    return r;
}

}  // namespace entropic
