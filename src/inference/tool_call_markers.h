// SPDX-License-Identifier: Apache-2.0
/**
 * @file tool_call_markers.h
 * @brief gh#103 (v2.8.2): family-aware tool-call CLOSE markers, derived from
 *        the resolved common_chat format.
 *
 * Pure mapping (no model, no I/O) so the vendor-coupled marker table is
 * CPU-unit-testable in isolation. A "sequential" tier appends the returned
 * marker to GenerationParams.stop, so the decode loop halts at the first closed
 * tool call (one tool per turn — observe-between-actions). The marker is RETAINED
 * in the generated content (check_stop_sequences does not truncate), so
 * common_chat still parses the complete `<tool_call>…</tool_call>` block.
 *
 * Markers track the vendored PEG parser defaults
 * (extern/llama.cpp/common/chat-peg-parser.cpp:442-443, `section_end`). They are
 * vendor-coupled and MUST be re-checked on a llama.cpp pin bump and are validated
 * empirically per family by the gh#103 model test. Any format without a
 * confirmed per-call close marker (CONTENT_ONLY, and — conservatively — GEMMA4,
 * whose section_end is not unambiguous in the vendored source) returns "" → the
 * orchestrator injects no stop and the tier keeps the default batch behavior
 * (terminal directives still halt the loop post-parse). "" is always safe: it
 * can never corrupt a parse or fire a spurious stop.
 *
 * @version 2.8.2
 */
#pragma once

#include <entropic/types/config.h>  // GenerationParams

#include <chat.h>

#include <algorithm>
#include <string>

namespace entropic {

/**
 * @brief Map a resolved common_chat format to its single-tool-call close marker.
 *
 * Single-return (knots returns ≤ 3). PEG_NATIVE / PEG_SIMPLE use the PEG
 * `section_end` default `</tool_call>` (qwen3 / hermes / nemotron-style). All
 * other formats return "" (no reliable per-call marker → batch-safe fallback).
 *
 * @param fmt Resolved common_chat format (from the last tool render).
 * @return Close marker string, or "" when none applies.
 * @utility
 * @version 2.8.2
 */
inline std::string close_marker_for_format(common_chat_format fmt) {
    std::string marker;
    switch (fmt) {
        case COMMON_CHAT_FORMAT_PEG_NATIVE:
        case COMMON_CHAT_FORMAT_PEG_SIMPLE:
            marker = "</tool_call>";
            break;
        case COMMON_CHAT_FORMAT_PEG_GEMMA4:
            // gh#103 (v2.8.2): gemma4 wraps a call as
            // `<|tool_call>call:NAME{...}<tool_call|>` — the per-call CLOSE is
            // `<tool_call|>` (confirmed against a live gemma4_e4b transcript +
            // chat.cpp:1178). Distinct from the open `<|tool_call>`, so the
            // ends-with stop check fires on the close, never the open. This is
            // the family where the missing hard-stop is most severe: the
            // runaway past the call defeats extraction, so the terminal
            // directive registers as ZERO tool calls (gh#103 consumer report).
            marker = "<tool_call|>";
            break;
        default:
            // CONTENT_ONLY / unknown: no confirmed per-call close marker —
            // return "" so no stop is injected (batch-safe).
            break;
    }
    return marker;
}

/**
 * @brief Append a tool-call close marker to params.stop for sequential mode.
 *
 * Pure engagement decision (no backend, no I/O) so it is CPU-unit-testable in
 * isolation — the instrumentation guard that FAILS if the gh#103 sequential
 * hard-stop does not engage (mirrors the gh#96 warm_keep_util decision helpers).
 * No-op unless `params.tool_call_mode == "sequential"` and `marker` is non-empty;
 * dedupes so an explicit per-call stop already carrying the marker is not
 * duplicated.
 *
 * @param params Generation params (stop list mutated).
 * @param marker Family close marker (from close_marker_for_format / the backend).
 * @utility
 * @version 2.8.2
 */
inline void append_sequential_stop(GenerationParams& params,
                                   const std::string& marker) {
    if (params.tool_call_mode != "sequential" || marker.empty()) { return; }
    if (std::find(params.stop.begin(), params.stop.end(), marker)
            == params.stop.end()) {
        params.stop.push_back(marker);
    }
}

}  // namespace entropic
