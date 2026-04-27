// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file tool_result_classify.h
 * @brief Byte-level classifiers for tool-result content (#44, v2.1.0).
 *
 * Used by ToolExecutor at the moment a server returns its
 * ServerResponse to map content into ToolResultKind. Pulled out of
 * tool_executor.cpp into a small header so the heuristics are
 * directly unit-testable.
 *
 * @par Design notes:
 * - "Empty" is purely byte-level (whitespace-only counts). The model's
 *   prompt is responsible for interpreting empty in context (e.g.
 *   filesystem.grep empty → pivot tools; bash.execute empty → likely
 *   side-effect-only success).
 * - Error detection covers the common "Error:" / "ERROR:" / "[error]"
 *   prefix shapes plus a top-level JSON ``{"error": …}`` sniff. False
 *   negatives previously surfaced as ToolResultKind::ok and let the
 *   model treat error text as success content.
 *
 * @version 2.1.0
 */

#pragma once

#include <entropic/entropic_export.h>

#include <string_view>

namespace entropic::mcp {

/**
 * @brief True if @p s is empty or contains only ASCII whitespace.
 *
 * @utility
 * @version 2.1.0
 */
ENTROPIC_EXPORT bool is_effectively_empty(std::string_view s);

/**
 * @brief Heuristic: the rendered tool-result text reads as an error.
 *
 * Recognises (case-insensitively, after stripping leading whitespace):
 *   - ``Error: …`` / ``ERROR …``
 *   - ``[error] …``
 *   - top-level ``{ "error": … }``
 *
 * Negative case: ``"… error somewhere in the middle"`` is NOT classified
 * as error — only leading-token shapes count.
 *
 * @utility
 * @version 2.1.0
 */
ENTROPIC_EXPORT bool looks_like_tool_error(std::string_view content);

} // namespace entropic::mcp
