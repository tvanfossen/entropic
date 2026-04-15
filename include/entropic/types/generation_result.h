// SPDX-License-Identifier: LGPL-3.0-or-later
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

} // namespace entropic
