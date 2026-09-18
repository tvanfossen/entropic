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

#include <entropic/types/config.h>
#include <entropic/types/generation_result.h>

#include <string>

namespace entropic {

/**
 * @brief Which source, if any, constrains the decode.
 * @version 2.10.4
 */
enum class GrammarSource {
    none,       ///< Unconstrained
    request,    ///< GenerationParams::grammar (COMMON_GRAMMAR_TYPE_USER)

    /// The TIER's identity frontmatter `grammar:` stem (gh#154). Reaches
    /// the sampler through `GenerationParams::grammar` exactly like
    /// `request` does — which is precisely why it needed naming: a
    /// consumer reading a result could not tell a tier-configured grammar
    /// from one they passed, or from none at all.
    tier,

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
 *
 * Reads the trailing `count` sentinel, so the value tracks the declaration
 * automatically: appending a source above the sentinel changes this number
 * and fails the invariant test, which is the tripwire's whole purpose.
 *
 * @return The sentinel's integer value — the count of declared sources
 *         including `none`, excluding the sentinel itself.
 * @req REQ-TYPE-004
 * @req REQ-INFER-008
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
 * @param request_from_tier true when the request-side text came from the
 *        tier's frontmatter `grammar:` stem rather than from the caller
 *        (gh#154). Reporting-only: the two are applied identically.
 * @return GrammarSource::request / ::tier when a request-side grammar is
 *         present, GrammarSource::tool_call when only the render-derived
 *         grammar is, GrammarSource::none when neither is.
 * @req REQ-INFER-008
 * @req REQ-TYPE-004
 * @version 2.13.0
 */
inline GrammarSource resolve_grammar_source(const std::string& request_grammar,
                                            const std::string& tool_grammar,
                                            bool request_from_tier = false) {
    GrammarSource source = GrammarSource::none;
    if (!request_grammar.empty()) {
        source = request_from_tier ? GrammarSource::tier
                                   : GrammarSource::request;
    } else if (!tool_grammar.empty()) {
        source = GrammarSource::tool_call;
    }
    return source;
}

/**
 * @brief Whether a source is applied as a request-side grammar.
 *
 * `tier` and `request` differ only in REPORTING — both arrive as
 * `GenerationParams::grammar` and are applied as COMMON_GRAMMAR_TYPE_USER.
 * The application site asks this rather than comparing against `request`
 * alone, so naming a further request-side source cannot silently stop the
 * grammar being applied.
 *
 * @param source Resolved source.
 * @return true for `request` and `tier`.
 * @req REQ-INFER-008
 * @version 2.13.0
 */
inline bool is_request_grammar(GrammarSource source) {
    return source == GrammarSource::request || source == GrammarSource::tier;
}

/**
 * @brief Stable wire name for a grammar source.
 * @param source Resolved source.
 * @return "none" | "request" | "tier" | "tool_call". Serialized into
 *         `entropic_metrics_json`, so these strings are a consumer contract.
 * @req REQ-INFER-008
 * @version 2.13.0
 */
inline const char* grammar_source_name(GrammarSource source) {
    const char* name = "none";
    if (source == GrammarSource::request) {
        name = "request";
    } else if (source == GrammarSource::tier) {
        name = "tier";
    } else if (source == GrammarSource::tool_call) {
        name = "tool_call";
    }
    return name;
}

/**
 * @brief Whether both sources are active — a config error worth reporting.
 * @param request_grammar GenerationParams::grammar.
 * @param tool_grammar Render-derived tool-call GBNF.
 * @return true when both are non-empty — the caller logs the collision at
 *         ERROR at the application site; false otherwise.
 * @req REQ-INFER-008
 * @version 2.10.4
 */
inline bool grammar_sources_collide(const std::string& request_grammar,
                                    const std::string& tool_grammar) {
    return !request_grammar.empty() && !tool_grammar.empty();
}

/**
 * @brief Describe what constrained one decode, for the result record (gh#154).
 *
 * Reads the SAME two inputs `apply_grammar_source` reads — the resolved
 * `params.grammar` and the render-derived tool grammar — through the same
 * `resolve_grammar_source`, so the record cannot disagree with what the
 * sampler did. That is the property worth having: a provenance field
 * computed from a parallel rule would be a second thing to keep in sync,
 * and gh#95 / gh#108 / gh#134 are all instances of that going wrong.
 *
 * A key that was NAMED but produced no text reports `resolved: false` with
 * the naming source still in `source` — the fail-open state a consumer
 * previously could only detect by a MISSING log line.
 *
 * @param params Resolved generation params (post resolve_grammar_key).
 * @param tool_grammar Render-derived tool-call GBNF ("" when none).
 * @return Provenance for `GenerationResult::grammar`.
 * @req REQ-INFER-008
 * @version 2.13.0
 */
inline GrammarProvenance describe_grammar(const GenerationParams& params,
                                          const std::string& tool_grammar) {
    const auto source = resolve_grammar_source(
        params.grammar, tool_grammar, params.grammar_from_tier);
    GrammarProvenance provenance;
    provenance.source = grammar_source_name(source);
    provenance.key = params.resolved_grammar_key;
    provenance.resolved = (source != GrammarSource::none);
    if (!provenance.resolved && !provenance.key.empty()) {
        provenance.source = params.grammar_from_tier
            ? grammar_source_name(GrammarSource::tier)
            : grammar_source_name(GrammarSource::request);
    }
    if (grammar_sources_collide(params.grammar, tool_grammar)) {
        provenance.conflict_winner = provenance.source;
    }
    return provenance;
}

} // namespace entropic
