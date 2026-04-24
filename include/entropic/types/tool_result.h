// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file tool_result.h
 * @brief Typed outcome for POST_TOOL_CALL hook consumers.
 *
 * Stable contract for the `result_kind` field on the POST_TOOL_CALL
 * hook context JSON. Clients branch on this enum's string form rather
 * than regex-matching engine-authored human-readable prose. Engine
 * wording (denial messages, error text) may change freely across
 * releases; these enum values do not.
 *
 * Precedent: mirrors the ValidationVerdict / relay_status pattern
 * introduced in rc17/rc18. (E10, 2.0.6-rc19)
 *
 * @version 2.0.6-rc19
 */

#pragma once

namespace entropic {

/**
 * @brief Categorical outcome of a single tool invocation.
 *
 * Values are serialized into the POST_TOOL_CALL hook context JSON as
 * the `result_kind` string. Do NOT reorder or remove values — clients
 * treat the string form as a stable contract.
 *
 * @version 2.0.6-rc19
 */
enum class ToolResultKind {
    ok = 0,                 ///< Tool dispatched, returned content
    error,                  ///< Tool server returned an error payload
    rejected_duplicate,     ///< Precondition: duplicate in recent history
    rejected_schema,        ///< Precondition: argument schema violation
    rejected_precondition,  ///< Any other precondition reject (auth, tier, hook-cancel)
};

/**
 * @brief Serialize a ToolResultKind to its wire-stable string form.
 * @param kind Enum value.
 * @return Static null-terminated string. Never NULL.
 * @utility
 * @version 2.0.6-rc19
 */
inline const char* result_kind_to_string(ToolResultKind kind) {
    const char* s = "ok";
    switch (kind) {
    case ToolResultKind::ok:                    s = "ok"; break;
    case ToolResultKind::error:                 s = "error"; break;
    case ToolResultKind::rejected_duplicate:    s = "rejected_duplicate"; break;
    case ToolResultKind::rejected_schema:       s = "rejected_schema"; break;
    case ToolResultKind::rejected_precondition: s = "rejected_precondition"; break;
    }
    return s;
}

} // namespace entropic
