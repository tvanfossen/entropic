// SPDX-License-Identifier: Apache-2.0
/**
 * @file grammar_source.h
 * @brief Which grammar source constrains a decode (gh#134).
 *
 * Entropic has two grammar sources and they cannot compose — they constrain
 * output to different languages:
 *
 *   - the REQUEST grammar (`GenerationParams::grammar`, also fed by
 *     `grammar_key` and identity `grammar:`)
 *   - the TOOL-CALL grammar llama.cpp derives from the staged tool schemas
 *     during `common_chat_templates_apply`
 *
 * Kept as a pure function with no vendor types so the precedence rule — the
 * only part carrying judgment — is unit-testable without a model. Before
 * v2.10.4 the tool-call source was discarded at the render entirely, so a
 * tools-staged tier decoded unconstrained; see llama_cpp_backend.cpp.
 *
 * @version 2.10.4
 */

#pragma once

#include <string>

namespace entropic {

/**
 * @brief Which source, if any, constrains the decode.
 * @version 2.10.4
 */
enum class GrammarSource {
    none,       ///< Unconstrained
    request,    ///< GenerationParams::grammar (COMMON_GRAMMAR_TYPE_USER)
    tool_call,  ///< Render-derived (COMMON_GRAMMAR_TYPE_TOOL_CALLS, needs prefill)

    /// Sentinel — MUST remain last. Adding a source above this line breaks
    /// grammar_source_invariant_test, which is the point: gh#95, gh#108 and
    /// gh#134 were each a grammar source that reached the engine without ever
    /// reaching the sampler, and each got only a bespoke regression test.
    /// A fifth source must not be able to ship unwired.
    count,
};

/**
 * @brief Number of declared grammar sources, sentinel excluded.
 * @utility
 * @version 2.10.4
 */
inline constexpr int grammar_source_count() {
    return static_cast<int>(GrammarSource::count);
}

/**
 * @brief Resolve which grammar wins.
 *
 * The request grammar takes precedence: it is an explicit caller instruction,
 * whereas the tool-call grammar is implied by staging tools. A caller that
 * supplies both has a config error — the collision is logged loudly at the
 * application site rather than silently resolved, because the losing
 * constraint's absence would otherwise be undiagnosable.
 *
 * @param request_grammar GenerationParams::grammar.
 * @param tool_grammar Render-derived tool-call GBNF.
 * @return The winning source.
 * @utility
 * @version 2.10.4
 */
inline GrammarSource resolve_grammar_source(const std::string& request_grammar,
                                            const std::string& tool_grammar) {
    if (!request_grammar.empty()) { return GrammarSource::request; }
    if (!tool_grammar.empty()) { return GrammarSource::tool_call; }
    return GrammarSource::none;
}

/**
 * @brief Whether both sources are active — a config error worth reporting.
 * @param request_grammar GenerationParams::grammar.
 * @param tool_grammar Render-derived tool-call GBNF.
 * @return true when both are non-empty.
 * @utility
 * @version 2.10.4
 */
inline bool grammar_sources_collide(const std::string& request_grammar,
                                    const std::string& tool_grammar) {
    return !request_grammar.empty() && !tool_grammar.empty();
}

} // namespace entropic
