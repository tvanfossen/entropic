// SPDX-License-Identifier: Apache-2.0
/**
 * @file mcp_key_set.cpp
 * @brief MCPKeySet implementation.
 * @version 1.9.4
 */

#include <entropic/mcp/mcp_key_set.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

static auto logger = entropic::log::get("mcp.key_set");

namespace entropic {

/**
 * @brief Grant a tool key with an access level.
 * @param pattern Tool pattern string.
 * @param level Access level to grant.
 * @utility
 * @version 1.9.4
 */
void MCPKeySet::grant(const std::string& pattern,
                      MCPAccessLevel level) {
    std::lock_guard<std::mutex> lock(key_mutex_);
    keys_[pattern] = level;
    logger->info("Granted {}:{}", pattern,
                 mcp_access_level_name(level));
}

/**
 * @brief Revoke a tool key entirely.
 * @param pattern Tool pattern string to revoke.
 * @return true if pattern was found and removed.
 * @utility
 * @version 1.9.4
 */
bool MCPKeySet::revoke(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(key_mutex_);
    auto it = keys_.find(pattern);
    if (it == keys_.end()) {
        return false;
    }
    keys_.erase(it);
    logger->info("Revoked {}", pattern);
    return true;
}

/**
 * @brief Check if a specific tool is authorized at the required level.
 *
 * Levels are ordinal, so a key granting WRITE also satisfies a READ
 * requirement but not the reverse — which is what makes ToolBase's
 * WRITE default a genuine restriction on a READ-only key set.
 *
 * @param tool_name Fully-qualified tool name.
 * @param required Minimum access level needed, as declared by the tool.
 * @return true when the best-matching granted key's level is at least
 *         `required`; false otherwise, including when no key matches
 *         (which resolves to NONE).
 * @req REQ-MCP-010
 * @req REQ-MCP-011
 * @version 1.9.4
 */
bool MCPKeySet::has_access(const std::string& tool_name,
                           MCPAccessLevel required) const {
    std::lock_guard<std::mutex> lock(key_mutex_);
    auto granted = find_best_match(tool_name);
    return static_cast<uint8_t>(granted) >=
           static_cast<uint8_t>(required);
}

/**
 * @brief List all granted keys.
 * @return Vector of MCPKey entries.
 * @utility
 * @version 1.9.4
 */
std::vector<MCPKey> MCPKeySet::list() const {
    std::lock_guard<std::mutex> lock(key_mutex_);
    std::vector<MCPKey> result;
    result.reserve(keys_.size());
    for (const auto& [pattern, level] : keys_) {
        result.push_back(MCPKey{pattern, level});
    }
    return result;
}

/**
 * @brief Number of granted keys.
 * @return Key count.
 * @utility
 * @version 1.9.4
 */
size_t MCPKeySet::size() const {
    std::lock_guard<std::mutex> lock(key_mutex_);
    return keys_.size();
}

/**
 * @brief Remove all granted keys.
 * @utility
 * @version 1.9.4
 */
void MCPKeySet::clear() {
    std::lock_guard<std::mutex> lock(key_mutex_);
    keys_.clear();
}

/**
 * @brief Serialize key set to JSON string.
 * @return JSON array of {pattern, level} objects.
 * @utility
 * @version 1.9.4
 */
std::string MCPKeySet::serialize() const {
    std::lock_guard<std::mutex> lock(key_mutex_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [pattern, level] : keys_) {
        arr.push_back({
            {"pattern", pattern},
            {"level", mcp_access_level_name(level)}
        });
    }
    return arr.dump();
}

/**
 * @brief Deserialize key set from JSON string.
 * @param json JSON array string.
 * @return true if parsed successfully.
 * @utility
 * @version 1.9.4
 */
bool MCPKeySet::deserialize(const std::string& json) {
    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& e) {
        logger->warn("Deserialize failed: {}", e.what());
        return false;
    }
    if (!arr.is_array()) {
        logger->warn("Deserialize: expected JSON array");
        return false;
    }
    std::lock_guard<std::mutex> lock(key_mutex_);
    keys_.clear();
    for (const auto& entry : arr) {
        if (!entry.contains("pattern") || !entry.contains("level")) {
            logger->warn("Skipping entry: missing fields");
            continue;
        }
        MCPAccessLevel level{};
        auto level_str = entry["level"].get<std::string>();
        if (!parse_mcp_access_level(level_str, level)) {
            logger->warn("Skipping entry: unknown level '{}'",
                         level_str);
            continue;
        }
        keys_[entry["pattern"].get<std::string>()] = level;
    }
    return true;
}

// ── Private helpers ──────────────────────────────────────

/**
 * @brief Extract server prefix wildcard from tool name.
 * @param tool_name E.g., "filesystem.read_file".
 * @return E.g., "filesystem.*", or empty if no dot.
 * @req REQ-MCP-010
 * @version 1.9.4
 */
std::string MCPKeySet::server_wildcard(
    const std::string& tool_name) {
    auto dot = tool_name.find('.');
    if (dot == std::string::npos) {
        return "";
    }
    return tool_name.substr(0, dot) + ".*";
}

/**
 * @brief Find the best matching access level for a tool name.
 *
 * Most-specific-wins: exact tool name, then the server wildcard
 * (`filesystem.*`), then the full wildcard (`*`).
 *
 * @param tool_name Fully-qualified tool name.
 * @return The granted MCPAccessLevel from the most specific matching
 *         key; MCPAccessLevel::NONE when nothing matches — default-deny
 *         once an identity is enforced.
 * @req REQ-MCP-010
 * @version 1.9.4
 */
MCPAccessLevel MCPKeySet::find_best_match(
    const std::string& tool_name) const {
    auto result = MCPAccessLevel::NONE;
    // 1. Exact match (most specific)
    auto it = keys_.find(tool_name);
    if (it != keys_.end()) {
        result = it->second;
    } else if (auto wc = server_wildcard(tool_name);
               !wc.empty() &&
               (it = keys_.find(wc)) != keys_.end()) {
        // 2. Server wildcard (e.g., "filesystem.*")
        result = it->second;
    } else if ((it = keys_.find("*")) != keys_.end()) {
        // 3. Full wildcard
        result = it->second;
    }
    return result;
}

} // namespace entropic
