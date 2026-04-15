// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file config.cpp
 * @brief Utility functions for config types.
 * @version 1.9.4
 */

#include <entropic/types/config.h>

#include <cstring>

namespace entropic {

/**
 * @brief Convert MCPAccessLevel to string representation.
 * @param level Access level.
 * @return Static string: "NONE", "READ", or "WRITE".
 * @utility
 * @version 1.9.4
 */
const char* mcp_access_level_name(MCPAccessLevel level) {
    static const char* const names[] = {"NONE", "READ", "WRITE"};
    auto idx = static_cast<int>(level);
    return (idx >= 0 && idx <= 2) ? names[idx] : "UNKNOWN";
}

/**
 * @brief Parse MCPAccessLevel from string.
 * @param name String: "NONE", "READ", or "WRITE" (case-sensitive).
 * @param out Parsed access level.
 * @return true if parsed successfully.
 * @utility
 * @version 1.9.4
 */
bool parse_mcp_access_level(const std::string& name,
                            MCPAccessLevel& out) {
    struct Entry { const char* str; MCPAccessLevel lvl; };
    static const Entry table[] = {
        {"WRITE", MCPAccessLevel::WRITE},
        {"READ",  MCPAccessLevel::READ},
        {"NONE",  MCPAccessLevel::NONE},
    };
    for (const auto& e : table) {
        if (name == e.str) { out = e.lvl; return true; }
    }
    return false;
}

} // namespace entropic
