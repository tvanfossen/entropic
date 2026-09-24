// SPDX-License-Identifier: Apache-2.0
/**
 * @file generation_result.h
 * @brief Generation output with metrics.
 *
 * Returned by InferenceBackend::generate() and related methods. Contains
 * the generated content, token counts, timing, and optional tool calls
 * parsed by the adapter.
 *
 * @version 1.8.2
 */

#pragma once

#include <entropic/types/error.h>
#include <entropic/types/tool_call.h>

#include <string>
#include <vector>

namespace entropic {

/**
 * @brief What constrained one decode, and what asked for it (gh#154).
 *
 * The engine always knew this and no consumer could ask. `GrammarRegistry::
 * get()` returns "" on a miss, the engine logs a warning, and the decode
 * proceeds UNCONSTRAINED — so from outside, a constrained run and an
 * unconstrained one produce output of the same shape whenever the prompt
 * also describes the shape. The only distinguishing signal was the ABSENCE
 * of a log line, which is not something a consumer can assert on; three days
 * of speculative-decode measurements were published against it and
 * withdrawn.
 *
 * `source` names who ASKED. `resolved` says whether a grammar text actually
 * reached the sampler. They are separate on purpose: a named key that does
 * not resolve is exactly the state that was undiagnosable.
 *
 * @version 2.13.0
 */
struct GrammarProvenance {
    /// @brief "request" | "tier" | "tool_call" | "none".
    std::string source = "none";

    /// @brief Registry key / frontmatter stem that was named ("" = none).
    std::string key;

    /// @brief true when a grammar text actually constrained this decode.
    bool resolved = false;

    /// @brief The winning source when two grammars collided ("" = no
    ///        collision). A request grammar displacing a tool-call grammar
    ///        means the staged tools were NOT structurally enforced.
    std::string conflict_winner;
};

/**
 * @brief Result of a single generation call.
 *
 * Maps to Python GenerationResult dataclass.
 *
 * @version 1.8.2
 */
struct GenerationResult {
    std::string content;                   ///< Generated text (cleaned by adapter)
    std::string raw_content;               ///< Raw model output before adapter processing
    std::vector<ToolCall> tool_calls;       ///< Tool calls parsed from content
    std::string finish_reason = "stop";    ///< Finish reason: "stop", "length", "error"
    int token_count = 0;                   ///< Generated token count
    double generation_time_ms = 0.0;       ///< Wall-clock generation time

    /// @brief Prompt tokens actually pushed through llama_decode during this
    ///        run's prefill (gh#144, v2.12.0).
    ///
    /// The instrumentation every prefill-reuse claim rests on. A correctness
    /// test passes via full-reprefill fallback even when reuse is completely
    /// dead, so "the answer was right" says nothing about whether a prefix
    /// was reused — this does. Zero when the path does not report it.
    /// @version 2.12.0
    int prefill_tokens = 0;

    /* ── Orchestrator timing (populated by ModelOrchestrator) ── */
    double routing_ms = 0.0;               ///< Router classification time
    double swap_ms = 0.0;                  ///< Model swap time
    double total_ms = 0.0;                 ///< Total end-to-end time

    /* ── v1.9.7: Throughput + time cap metadata ── */

    /// @brief Measured throughput for this generation (tok/s).
    /// Computed from token_count / generation_time_ms. 0.0 if either is 0.
    /// @version 1.9.7
    double throughput_tok_s = 0.0;

    /// @brief true if generation was terminated by time limit rather than
    /// EOS/stop sequence/max_tokens.
    /// @version 1.9.7
    bool time_limited = false;

    /// @brief Original max_tokens before auto-adaptation reduced it.
    /// 0 if no adaptation occurred.
    /// @version 1.9.7
    int original_max_tokens = 0;

    /* ── v1.9.13: Multi-sequence tracking ── */

    /// @brief Sequence identifier for multi-sequence backends.
    /// 0 for single-sequence backends (default). Set by generate_seq()
    /// to track which sequence produced this result.
    /// @version 1.9.13
    int seq_id = 0;

    /* ── gh#36 / gh#106: speculative-decode observability ── */

    /// @brief Tokens proposed by the draft/MTP head across all rounds.
    /// 0 when no speculative kernel ran (plain decode). The instrumentation
    /// signal that MTP/speculative actually engaged vs silently fell back.
    /// @version 2.9.0
    int n_drafted = 0;

    /// @brief Draft tokens the target accepted (≤ n_drafted). >0 proves the
    /// head's drafts are lossless-verified hits, not just proposed.
    /// @version 2.9.0
    int n_accepted = 0;

    /* ── gh#154: grammar provenance ── */

    /// @brief What constrained this decode and who asked for it.
    /// Populated by the orchestrator from the SAME inputs the sampler's
    /// application site uses, so the record cannot disagree with what ran.
    /// @version 2.13.0
    GrammarProvenance grammar;

    /* ── Error state (for partial results on failure) ── */
    entropic_error_t error_code = ENTROPIC_OK; ///< Error code (ENTROPIC_OK if no error)
    std::string error_message;              ///< Error description (empty if no error)

    /**
     * @brief True if generation completed without error.
     * @return true when error_code == ENTROPIC_OK.
     * @utility
     * @version 1.8.2
     */
    bool ok() const { return error_code == ENTROPIC_OK; }
};

/**
 * @brief One generation's metrics, without its text (gh#154).
 *
 * `GenerationResult` is rich and per-call; this is what survives the call
 * and reaches a consumer through `entropic_metrics_json`. Content and
 * tool calls are deliberately excluded — the engine keeps a bounded ring
 * of these, and keeping the text would make it a transcript.
 *
 * @version 2.13.0
 */
struct GenerationRecord {
    std::string finish_reason;        ///< "stop", "length", "error"
    int token_count = 0;              ///< Tokens generated
    int prefill_tokens = 0;           ///< Prompt tokens actually decoded (gh#144)
    double throughput_tok_s = 0.0;    ///< Measured decode throughput
    int n_drafted = 0;                ///< Speculative/MTP tokens proposed
    int n_accepted = 0;               ///< Of those, accepted by the target
    GrammarProvenance grammar;        ///< What constrained the decode
};

/**
 * @brief Project a result down to the record kept for metrics.
 * @param r Completed generation result.
 * @return The metric fields, without content or tool calls.
 * @utility
 * @version 2.13.0
 */
inline GenerationRecord make_generation_record(const GenerationResult& r) {
    GenerationRecord rec;
    rec.finish_reason = r.finish_reason;
    rec.token_count = r.token_count;
    rec.prefill_tokens = r.prefill_tokens;
    rec.throughput_tok_s = r.throughput_tok_s;
    rec.n_drafted = r.n_drafted;
    rec.n_accepted = r.n_accepted;
    rec.grammar = r.grammar;
    return rec;
}

} // namespace entropic
