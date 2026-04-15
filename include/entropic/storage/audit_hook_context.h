// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file audit_hook_context.h
 * @brief AuditHookContext — bridges engine state to audit hook callback.
 *
 * Owned by the engine, passed as user_data to the POST_TOOL_CALL hook.
 * Contains pointers to live engine state so the audit callback can
 * read caller_id, delegation_depth, etc. without coupling to engine
 * internals.
 *
 * @version 1.9.5
 */

#pragma once

#include <string>

namespace entropic {

class AuditLogger;

/**
 * @brief Context passed to AuditLogger hook via user_data pointer.
 *
 * The engine updates these fields before firing POST_TOOL_CALL.
 * The AuditLogger reads them in hook_callback(). This avoids
 * passing engine internals through the hook JSON.
 *
 * @version 1.9.5
 */
struct AuditHookContext {
    AuditLogger* logger = nullptr;                   ///< Logger instance
    std::string session_id;                          ///< Current session UUID
    const std::string* caller_id = nullptr;          ///< Pointer to current identity name
    const int* delegation_depth = nullptr;           ///< Pointer to current depth
    const int* iteration = nullptr;                  ///< Pointer to current iteration
    const std::string* parent_conversation_id = nullptr; ///< Pointer to parent conv ID
};

struct AuditEntry;

/**
 * @brief Populate AuditEntry fields from AuditHookContext state.
 * @param entry Entry to populate (caller_id, depth, iteration, parent_id, status).
 * @param ctx Hook context with engine state pointers.
 * @internal
 * @version 1.9.5
 */
void populate_from_hook_context(AuditEntry& entry,
                                const AuditHookContext& ctx);

} // namespace entropic
