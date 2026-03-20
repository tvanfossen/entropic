/**
 * @file permission_manager.h
 * @brief Permission manager for MCP tool access control.
 *
 * Default-deny permission model. Tools must be explicitly allowed or
 * approved via callback. Deny list takes precedence over allow list.
 *
 * Pattern syntax (fnmatch semantics):
 * - "filesystem.*" — all filesystem tools
 * - "bash.execute:python*" — bash commands starting with "python"
 * - "git.commit" — git commit specifically
 *
 * @version 1.8.5
 */

#pragma once

#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Permission manager for MCP tool access control.
 *
 * Default-deny model. Deny list takes precedence over allow list.
 * Only returns false for is_denied() if a deny pattern explicitly
 * matches. Unknown tools are not denied — the engine handles
 * prompting for tools not in either list.
 *
 * @version 1.8.5
 */
class PermissionManager {
public:
    /**
     * @brief Construct with initial allow/deny lists.
     * @param allow_patterns Allow list patterns.
     * @param deny_patterns Deny list patterns.
     * @version 1.8.5
     */
    PermissionManager(std::vector<std::string> allow_patterns = {},
                      std::vector<std::string> deny_patterns = {});

    /**
     * @brief Check if a tool call is explicitly denied.
     * @param tool_name Fully-qualified tool name (e.g., "filesystem.read_file").
     * @param pattern Tool pattern with args (e.g., "filesystem.read_file:/path").
     * @return true if denied.
     * @version 1.8.5
     */
    bool is_denied(const std::string& tool_name,
                   const std::string& pattern) const;

    /**
     * @brief Check if a tool call is explicitly allowed (skip prompting).
     * @param tool_name Fully-qualified tool name.
     * @param pattern Tool pattern with args.
     * @return true if explicitly in allow list.
     * @version 1.8.5
     */
    bool is_allowed(const std::string& tool_name,
                    const std::string& pattern) const;

    /**
     * @brief Add a permission pattern at runtime.
     * @param pattern Permission pattern string.
     * @param allow true for allow list, false for deny list.
     * @version 1.8.5
     */
    void add_permission(const std::string& pattern, bool allow);

private:
    std::vector<std::string> allow_list_;  ///< Allow patterns
    std::vector<std::string> deny_list_;   ///< Deny patterns

    /**
     * @brief Check if a tool matches a permission pattern (fnmatch semantics).
     * @param tool_name Fully-qualified tool name.
     * @param full_pattern Tool name with args pattern.
     * @param permission_pattern Permission pattern to test against.
     * @return true if matches.
     * @version 1.8.5
     */
    static bool pattern_matches(const std::string& tool_name,
                                const std::string& full_pattern,
                                const std::string& permission_pattern);
};

} // namespace entropic
