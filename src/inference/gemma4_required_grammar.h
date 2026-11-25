// SPDX-License-Identifier: Apache-2.0
/**
 * @file gemma4_required_grammar.h
 * @brief Root-rule patch for the Gemma 4 tool_choice=REQUIRED grammar (v2.14.6).
 *
 * At the vendored llama.cpp pin (fb27a525d), common_chat_params_init_gemma4
 * builds the REQUIRED tool-call grammar with root
 * `start message* scan-to-toolcall tool-call`. Every `until-*` rule in the
 * GBNF has an empty alternative, so `content` can end mid-delimiter and the
 * `message*` loop restarts it: a delimiter such as `<|tool_call>` is matched
 * as `<|tool_call` followed by `>`. The loop therefore accepts any text, and
 * the grammar's only effect is to mask EOG until the text ends in a valid
 * call. Measured on E4B: a call written with plain `"` strings is never a
 * valid call, so EOG stays masked and the turn runs to max_tokens.
 *
 * Upstream fixed this in f072b10371 (#29115, "chat : fix gemma4 required
 * tool grammar"): under REQUIRED the root is `start + thought + tool_call`.
 * entropic does not patch extern/, so this header rewrites the root rule of
 * the rendered GBNF to the same shape. Pure string functions, no vendor
 * types, so the rewrite is unit-testable without a model.
 *
 * @version 2.14.6
 */

#pragma once

#include <string>

namespace entropic {

/**
 * @brief The REQUIRED root rule as the pre-f072b10371 pin renders it.
 * @return The exact root line this patch recognises.
 * @utility
 * @version 2.14.6
 */
inline const char* gemma4_required_root_prefix_shape() {
    return "root ::= start message* scan-to-toolcall tool-call";
}

/**
 * @brief The root rule after the patch, mirroring f072b10371.
 *
 * `(thought | )` is the optional-thought alternative exactly as the pinned
 * GBNF spells it in the `message` rule (the PEG's peek/negate guards do not
 * survive translation to GBNF).
 *
 * @return The replacement root line.
 * @utility
 * @version 2.14.6
 */
inline const char* gemma4_required_root_fixed_shape() {
    return "root ::= start (thought | ) tool-call";
}

/**
 * @brief Offset of the `root ::=` line in a GBNF, or npos.
 * @param gbnf Grammar text.
 * @return Offset of the line start; std::string::npos when absent.
 * @utility
 * @version 2.14.6
 */
inline size_t gbnf_root_line_offset(const std::string& gbnf) {
    size_t at = gbnf.rfind("root ::=", 0) == 0 ? 0 : std::string::npos;
    if (at == std::string::npos) {
        const size_t nl = gbnf.find("\nroot ::=");
        at = nl == std::string::npos ? nl : nl + 1;
    }
    return at;
}

/**
 * @brief The full `root ::=` line of a GBNF (without its newline).
 * @param gbnf Grammar text.
 * @return The root line, or "" when the grammar has none.
 * @utility
 * @version 2.14.6
 */
inline std::string gbnf_root_line(const std::string& gbnf) {
    const size_t at = gbnf_root_line_offset(gbnf);
    std::string line;
    if (at != std::string::npos) {
        line = gbnf.substr(at, gbnf.find('\n', at) - at);
    }
    return line;
}

/**
 * @brief Rewrite a Gemma 4 REQUIRED grammar's root to the f072b10371 shape.
 *
 * Only the exact pre-fix root line is rewritten. Any other root shape,
 * including the already-fixed one a newer pin emits, is returned
 * unchanged: the caller compares input and output to tell which happened.
 *
 * @param gbnf GBNF from common_chat_params_init_gemma4 under REQUIRED.
 * @return The grammar with its root rewritten, or @p gbnf unchanged.
 * @req REQ-INFER-008
 * @version 2.14.6
 */
inline std::string patch_gemma4_required_root(const std::string& gbnf) {
    std::string out = gbnf;
    const std::string old_line = gemma4_required_root_prefix_shape();
    if (gbnf_root_line(gbnf) == old_line) {
        out.replace(gbnf_root_line_offset(gbnf), old_line.size(),
                    gemma4_required_root_fixed_shape());
    }
    return out;
}

} // namespace entropic
