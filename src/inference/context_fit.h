// SPDX-License-Identifier: Apache-2.0
/**
 * @file context_fit.h
 * @brief v2.13.0 prompt-vs-context admission arithmetic for one turn.
 *
 * Pure functions (no llama.cpp, no I/O) so the refusal decision and the
 * operator-facing diagnosis are unit-testable on the CPU pre-commit tier.
 * The backend supplies the measurements; this decides whether the turn can
 * run and, when it cannot, says which knob the operator has to turn.
 *
 * Found by the v2.13.0 release gate: a handle staged 27 tools (18,594 bytes)
 * into a tier configured `context_length: 2048`, rendered a 5,026-token
 * prompt, and llama.cpp answered every turn with
 * `Decode chunk failed (slot=0, start=287, off=1536, chunk=512)` followed by
 * `Cache restore failed, falling back to full prefill`. The engine then
 * burned its empty-turn allowance on a prompt that could never have fitted.
 * Same fail-open class as gh#95 and gh#154: the engine knew both numbers and
 * proceeded anyway.
 *
 * @version 2.13.0
 */
#ifndef ENTROPIC_INFERENCE_CONTEXT_FIT_H
#define ENTROPIC_INFERENCE_CONTEXT_FIT_H

#include <cstddef>
#include <string>

namespace entropic {

/**
 * @brief One turn's irreducible prompt, measured against the tier budget.
 *
 * "Irreducible" is what the backend is handed AFTER the engine's context
 * management has done everything it can — `check_compaction` and
 * `prune_old_tool_results` both run before the generate dispatch. The staged
 * tool block is the part compaction provably cannot shrink: it is rebuilt
 * from tier config on every render.
 *
 * @version 2.13.0
 */
struct ContextFit {
    int prompt_tokens = 0;       ///< Rendered + tokenized prompt for this turn
    int context_length = 0;      ///< Tier's PER-SESSION context window
    int system_tokens = 0;       ///< Tokens attributable to the system prompt
    int tool_tokens = 0;         ///< Tokens attributable to the staged tools
    std::size_t tool_bytes = 0;  ///< Raw bytes of the staged tool JSON
    int tool_count = 0;          ///< Number of staged tool definitions
};

/**
 * @brief Whether this turn's prompt cannot fit the tier's context at all.
 *
 * The test is deliberately the unarguable one: a prompt at or beyond
 * `context_length` leaves the model zero positions to emit into, so no
 * downstream behaviour can rescue it. It does NOT reserve room for
 * `max_tokens` — a prompt that fits but leaves less headroom than the
 * caller asked for is a "length" finish, not a refusal, and clamping it
 * silently is the very thing this guard exists to stop.
 *
 * @param f Measured prompt sizing.
 * @return true when the turn must be refused before any decode.
 * @req REQ-INFER-026
 * @version 2.13.0
 */
inline bool context_fit_overflows(const ContextFit& f) {
    return f.context_length > 0 && f.prompt_tokens >= f.context_length;
}

/**
 * @brief Tokens left over once system prompt and tool block are accounted.
 * @param f Measured prompt sizing.
 * @return Non-negative remainder attributed to the message history.
 * @utility
 * @version 2.13.0
 */
inline int context_fit_history_tokens(const ContextFit& f) {
    const int rest = f.prompt_tokens - f.system_tokens - f.tool_tokens;
    return rest > 0 ? rest : 0;
}

/**
 * @brief Name the single largest contributor to the overflow.
 *
 * This is the whole point of the diagnosis: tool block, system prompt and
 * history each have a DIFFERENT fix (narrow `allowed_tools`, shorten the
 * identity, raise `context_length`), and an operator told only the total
 * has to guess which one.
 *
 * @param f Measured prompt sizing.
 * @return Human-readable name of the dominant contributor.
 * @utility
 * @version 2.13.0
 */
inline std::string context_fit_largest_contributor(const ContextFit& f) {
    int most = context_fit_history_tokens(f);
    std::string who = "the message history";
    if (f.tool_tokens > most) {
        most = f.tool_tokens;
        who = "the staged tool block";
    }
    if (f.system_tokens > most) {
        who = "the system prompt";
    }
    return who;
}

/**
 * @brief Per-contributor token breakdown, as one clause.
 * @param f Measured prompt sizing.
 * @return "staged tools N tok (M tools, B bytes), system prompt ... ".
 * @utility
 * @version 2.13.0
 */
inline std::string context_fit_breakdown(const ContextFit& f) {
    return "staged tools " + std::to_string(f.tool_tokens) + " tok ("
         + std::to_string(f.tool_count) + " tools, "
         + std::to_string(f.tool_bytes) + " bytes); system prompt "
         + std::to_string(f.system_tokens) + " tok; message history "
         + std::to_string(context_fit_history_tokens(f)) + " tok";
}

/**
 * @brief The refusal an operator reads, carrying every number they need.
 *
 * Names the token count, the tier's `context_length`, the overshoot, the
 * dominant contributor and the full breakdown — then the two knobs that
 * can actually change the outcome. No silent trim, no auto-raise.
 *
 * @param f Measured prompt sizing (expected to satisfy
 *        `context_fit_overflows`).
 * @return One-line diagnosis for the error result and the ERROR log.
 * @req REQ-INFER-026
 * @version 2.13.0
 */
inline std::string context_overflow_message(const ContextFit& f) {
    return "Prompt does not fit this tier's context: "
         + std::to_string(f.prompt_tokens) + " prompt tokens against "
           "context_length=" + std::to_string(f.context_length)
         + " (over by "
         + std::to_string(f.prompt_tokens - f.context_length)
         + " tokens). Largest contributor: "
         + context_fit_largest_contributor(f) + ". Breakdown: "
         + context_fit_breakdown(f)
         + ". Refused before decode — compaction cannot shrink the staged "
           "tool block. Raise this tier's context_length, or narrow its "
           "allowed_tools.";
}

}  // namespace entropic

#endif  // ENTROPIC_INFERENCE_CONTEXT_FIT_H
