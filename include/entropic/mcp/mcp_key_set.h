// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file mcp_key_set.h
 * @brief Per-identity set of authorized MCP tool keys.
 *
 * Default state is EMPTY — no tools authorized. Keys must be explicitly
 * granted. This enforces the default-deny principle at the identity level.
 *
 * Tool patterns support:
 * - Exact match: "filesystem.read_file"
 * - Server wildcard: "filesystem.*"
 * - Full wildcard: "*" (all tools — use sparingly)
 *
 * Grant resolution: most specific pattern wins. "filesystem.read_file:WRITE"
 * overrides "filesystem.*:READ" for that specific tool.
 *
 * @par Thread safety
 * grant/revoke acquire key_mutex_. has_access() acquires key_mutex_ for
 * consistency. Serialization acquires key_mutex_.
 *
 * @version 1.9.4
 */

#pragma once

#include <entropic/types/config.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Per-identity set of authorized MCP tool keys.
 *
 * Manages a map of tool patterns to access levels. Pattern matching
 * uses three-tier resolution (exact → server wildcard → full wildcard)
 * with O(1) lookup per tier.
 *
 * @version 1.9.4
 */
class MCPKeySet {
public:
    /**
     * @brief Grant a tool key with an access level.
     * @param pattern Tool pattern string.
     * @param level Access level to grant.
     * @version 1.9.4
     *
     * If the pattern already exists, the level is updated (not additive).
     * Granting READ to a pattern that had WRITE downgrades it.
     */
    void grant(const std::string& pattern, MCPAccessLevel level);

    /**
     * @brief Revoke a tool key entirely.
     * @param pattern Tool pattern string to revoke.
     * @return true if pattern was found and removed.
     * @version 1.9.4
     */
    bool revoke(const std::string& pattern);

    /**
     * @brief Check if a specific tool is authorized at the required level.
     * @param tool_name Fully-qualified tool name (e.g., "filesystem.read_file").
     * @param required Minimum access level needed.
     * @return true if authorized.
     * @version 1.9.4
     *
     * Resolution order (most specific wins):
     * 1. Exact match on tool_name
     * 2. Server wildcard (e.g., "filesystem.*" matches "filesystem.read_file")
     * 3. Full wildcard ("*")
     * 4. No match → denied
     *
     * Access level comparison: granted >= required means authorized.
     */
    bool has_access(const std::string& tool_name,
                    MCPAccessLevel required) const;

    /**
     * @brief List all granted keys.
     * @return Vector of MCPKey entries.
     * @version 1.9.4
     */
    std::vector<MCPKey> list() const;

    /**
     * @brief Number of granted keys.
     * @return Key count.
     * @version 1.9.4
     */
    size_t size() const;

    /**
     * @brief Remove all granted keys.
     * @version 1.9.4
     */
    void clear();

    /**
     * @brief Serialize key set to JSON string.
     * @return JSON array: [{"pattern":"...", "level":"WRITE"}, ...]
     * @version 1.9.4
     */
    std::string serialize() const;

    /**
     * @brief Deserialize key set from JSON string.
     * @param json JSON array string (same format as serialize output).
     * @return true if parsed successfully.
     * @version 1.9.4
     *
     * Replaces all current keys with the deserialized set.
     * Invalid entries are logged as WARNING and skipped.
     */
    bool deserialize(const std::string& json);

private:
    /// @brief Granted keys, keyed by pattern for O(1) exact lookup.
    std::unordered_map<std::string, MCPAccessLevel> keys_;

    /// @brief Guards mutations to keys_ map.
    mutable std::mutex key_mutex_;

    /**
     * @brief Extract server prefix from a fully-qualified tool name.
     * @param tool_name Tool name (e.g., "filesystem.read_file").
     * @return Server prefix with wildcard (e.g., "filesystem.*"), or empty.
     * @internal
     * @version 1.9.4
     */
    static std::string server_wildcard(const std::string& tool_name);

    /**
     * @brief Find the best matching access level for a tool name.
     * @param tool_name Fully-qualified tool name.
     * @return Granted MCPAccessLevel, or NONE if no match.
     * @internal
     * @version 1.9.4
     */
    MCPAccessLevel find_best_match(const std::string& tool_name) const;
};

} // namespace entropic
