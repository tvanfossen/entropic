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

    /* ── Error state (for partial results on failure) ── */
    entropic_error_t error_code = ENTROPIC_OK; ///< Error code (ENTROPIC_OK if no error)
    std::string error_message;              ///< Error description (empty if no error)

    /// @brief True if generation completed without error.
    /// @version 1.8.2
    bool ok() const { return error_code == ENTROPIC_OK; }
};

} // namespace entropic
