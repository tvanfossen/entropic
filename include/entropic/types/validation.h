// SPDX-License-Identifier: Apache-2.0
/**
 * @file validation.h
 * @brief Constitutional validation types — critique results and violations.
 *
 * Data types produced by the constitutional validation pipeline.
 * Used by ConstitutionalValidator (core.so) and returned through the
 * C API via entropic_validation_last_result().
 *
 * @version 1.9.8
 */

#pragma once

#include <string>
#include <vector>

namespace entropic {

/**
 * @brief A single constitutional violation found during critique.
 *
 * Each violation identifies which constitutional rule was broken,
 * quotes the offending excerpt, and explains why it violates the rule.
 *
 * @version 1.9.8
 */
struct Violation {
    std::string rule;          ///< Name of the violated constitutional rule
    std::string excerpt;       ///< Excerpt from the output that violates the rule
    std::string explanation;   ///< Why this excerpt violates the rule
};

/**
 * @brief Structured result from a single critique generation pass.
 *
 * Produced by parsing the grammar-constrained JSON output of the
 * critique generation. The grammar ensures valid JSON structure;
 * parse failures are handled as infrastructure errors (not content
 * judgments).
 *
 * @version 1.9.8
 */
struct CritiqueResult {
    bool compliant = true;                ///< true if output passes all rules
    std::vector<Violation> violations;    ///< List of constitutional violations
    std::string revised;                  ///< Revised text (may be empty)
    std::string raw_json;                 ///< Raw critique JSON for audit logging
};

/**
 * @brief Result of the full validation pipeline (critique + optional revision).
 *
 * Encapsulates the final output after zero or more revision attempts.
 * Returned by ConstitutionalValidator::validate() and exposed through
 * the C API as JSON via entropic_validation_last_result().
 *
 * @version 1.9.8
 */
/**
 * @brief Outcome of a validation pipeline run.
 *
 * Lets callers distinguish clean pass from safety-valve revert and
 * from max-revisions-exhausted. Surfaces through ON_COMPLETE hook
 * context and async task status so consumers know whether the
 * returned content is trustworthy. (E1 / 2.0.6-rc17)
 *
 * @version 2.0.6-rc17
 */
enum class ValidationVerdict {
    passed = 0,                ///< No violations, content unchanged
    revised,                   ///< Violations found; revision applied
    rejected_reverted_length,  ///< Revision gutted content >50%; original preserved
    rejected_max_revisions,    ///< Revisions exhausted; last output returned as-is
    skipped,                   ///< Validation did not run (skip_tiers / pure-tool-call / empty)
    /// @brief gh#30 (v2.1.5): auto_retry disabled and a critique failed.
    /// Engine paused before entering the revision loop; consumer must
    /// call entropic_validation_resume_retry() or _accept_last().
    paused_pending_consumer,
    /// @brief gh#30 (v2.1.5): consumer called accept_last() to override
    /// a paused rejection. Last attempt is the final answer.
    passed_consumer_override,
};

struct ValidationResult {
    std::string content;              ///< Final output (original or revised)
    bool was_revised = false;         ///< true if content differs from original
    int revision_count = 0;           ///< Number of revision attempts made
    CritiqueResult final_critique;    ///< Last critique result
    ValidationVerdict verdict =
        ValidationVerdict::passed;    ///< Structured outcome (2.0.6-rc17)
    /// @brief gh#30 (v2.1.5): attempt index this result corresponds to.
    /// 0 = original generation, 1 = first revision, etc. Same value as
    /// revision_count when the verdict is `revised` / `rejected_*`.
    int attempt_n = 0;
};

} // namespace entropic
