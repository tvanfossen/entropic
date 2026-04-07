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
 * @internal
 * @version 1.8.5
 */
PermissionManager::PermissionManager(
    std::vector<std::string> allow_patterns,
    std::vector<std::string> deny_patterns)
    : allow_list_(std::move(allow_patterns)),
      deny_list_(std::move(deny_patterns)) {}

/**
 * @brief Check if a tool call is explicitly denied.
 * @param tool_name Fully-qualified tool name.
 * @param pattern Tool pattern with args.
 * @return true if denied.
 * @internal
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
 * @param tool_name Fully-qualified tool name.
 * @param pattern Tool pattern with args.
 * @return true if in allow list.
 * @internal
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
 * @param pattern Permission pattern string.
 * @param allow true for allow list, false for deny list.
 * @internal
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
 * @param tool_name Fully-qualified tool name.
 * @param full_pattern Tool name with args pattern.
 * @param permission_pattern Permission pattern to test.
 * @return true if matches.
 * @internal
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
