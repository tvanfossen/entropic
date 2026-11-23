// SPDX-License-Identifier: Apache-2.0
/**
 * @file permission_manager.cpp
 * @brief PermissionManager implementation with fnmatch pattern matching.
 * @version 1.8.5
 */

#include <entropic/mcp/permission_manager.h>
#include <entropic/types/logging.h>

#include <fnmatch.h>

static auto logger = entropic::log::get("mcp.permissions");

namespace entropic {

/**
 * @brief Construct with initial allow/deny lists.
 * @param allow_patterns Allow list patterns.
 * @param deny_patterns Deny list patterns.
 * @utility
 * @version 1.8.5
 */
PermissionManager::PermissionManager(
    std::vector<std::string> allow_patterns,
    std::vector<std::string> deny_patterns)
    : allow_list_(std::move(allow_patterns)),
      deny_list_(std::move(deny_patterns)) {}

/**
 * @brief Check if a tool call is explicitly denied.
 *
 * Consulted before the allow list, so deny takes precedence whenever
 * both lists match the same call.
 *
 * @param tool_name Fully-qualified tool name (`<server>.<tool>`).
 * @param pattern The `<tool>:<args-summary>` pattern for this call.
 * @return true when any deny pattern matches (the match is logged);
 *         false when the deny list is empty or nothing matches. A false
 *         here is NOT an approval — the engine's callback still prompts.
 * @req REQ-MCP-009
 * @version 2.0.0
 */
bool PermissionManager::is_denied(
    const std::string& tool_name,
    const std::string& pattern) const {
    for (const auto& deny : deny_list_) {
        if (pattern_matches(tool_name, pattern, deny)) {
            logger->info("Permission DENIED: {} (matched '{}')",
                         tool_name, deny);
            return true;
        }
    }
    return false;
}

/**
 * @brief Check if a tool call is explicitly allowed.
 * @param tool_name Fully-qualified tool name (`<server>.<tool>`).
 * @param pattern The `<tool>:<args-summary>` pattern for this call.
 * @return true when any allow pattern matches (the match is logged);
 *         false otherwise. Callers must still honour is_denied(), which
 *         wins over any allow match.
 * @req REQ-MCP-009
 * @version 2.0.0
 */
bool PermissionManager::is_allowed(
    const std::string& tool_name,
    const std::string& pattern) const {
    for (const auto& allow : allow_list_) {
        if (pattern_matches(tool_name, pattern, allow)) {
            logger->info("Permission ALLOWED: {} (matched '{}')",
                         tool_name, allow);
            return true;
        }
    }
    return false;
}

/**
 * @brief Add a permission pattern at runtime.
 *
 * Backs the operator's "always allow / always deny" decision; the new
 * pattern takes effect on the next call. Re-adding an identical pattern
 * is a no-op so repeated approvals do not grow the list.
 *
 * @param pattern Permission pattern string, at whatever granularity the
 *                owning server's get_permission_pattern chose.
 * @param allow true to insert into the allow list, false for the deny
 *              list.
 * @req REQ-MCP-009
 * @version 1.8.5
 */
void PermissionManager::add_permission(
    const std::string& pattern, bool allow) {
    auto& list = allow ? allow_list_ : deny_list_;
    for (const auto& existing : list) {
        if (existing == pattern) {
            return;
        }
    }
    list.push_back(pattern);
    logger->info("Added {} permission: {}",
                 allow ? "allow" : "deny", pattern);
}

/**
 * @brief Check if a tool matches a permission pattern.
 *
 * fnmatch semantics on both halves, which is what lets one mechanism
 * express tool-level ("git.commit"), server-level ("filesystem.&#42;")
 * and argument-level ("bash.execute:python&#42;") patterns. A pattern
 * without a ':' is satisfied by the tool-name match alone.
 *
 * @param tool_name Fully-qualified tool name (`<server>.<tool>`).
 * @param full_pattern This call's `<tool>:<args-summary>` pattern.
 * @param permission_pattern Configured or runtime-added pattern to test.
 * @return true when the tool half matches and, for an argument-level
 *         pattern, the full `<tool>:<args>` string matches too; false
 *         otherwise.
 * @req REQ-MCP-009
 * @version 1.8.5
 */
bool PermissionManager::pattern_matches(
    const std::string& tool_name,
    const std::string& full_pattern,
    const std::string& permission_pattern) {

    // Split permission pattern at ':'
    auto colon = permission_pattern.find(':');
    std::string pattern_tool = (colon != std::string::npos)
        ? permission_pattern.substr(0, colon)
        : permission_pattern;

    // Tool name must match the tool portion
    if (fnmatch(pattern_tool.c_str(), tool_name.c_str(), 0) != 0) {
        return false;
    }

    // If no arg pattern, tool match is sufficient
    if (colon == std::string::npos) {
        return true;
    }

    // Full pattern must match
    return fnmatch(permission_pattern.c_str(),
                   full_pattern.c_str(), 0) == 0;
}

} // namespace entropic
