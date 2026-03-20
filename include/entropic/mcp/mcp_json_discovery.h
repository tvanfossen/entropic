/**
 * @file mcp_json_discovery.h
 * @brief Discovers external MCP servers from .mcp.json files.
 *
 * Searches project-level and global .mcp.json files. Produces
 * ExternalServerConfig entries ready for connection. Handles
 * self-detection (skip own socket) and shadow warnings.
 *
 * @version 1.8.7
 */

#pragma once

#include <entropic/types/config.h>

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Parsed server entry from .mcp.json.
 * @version 1.8.7
 */
struct ExternalServerConfig {
    std::string name;                              ///< Server name
    std::string transport;                         ///< "stdio" | "sse"
    std::string command;                           ///< Stdio command (empty for SSE)
    std::vector<std::string> args;                 ///< Stdio command args
    std::map<std::string, std::string> env;        ///< Stdio env vars
    std::string url;                               ///< SSE URL (empty for stdio)
};

/**
 * @brief Discovers external MCP servers from .mcp.json files.
 *
 * Two locations searched (first match per name wins):
 *   1. {project_dir}/.mcp.json — project-level
 *   2. ~/.entropic/.mcp.json — global fallback
 *
 * @version 1.8.7
 */
class MCPJsonDiscovery {
public:
    /**
     * @brief Construct with project directory.
     * @param project_dir Project root for .mcp.json search.
     * @version 1.8.7
     */
    explicit MCPJsonDiscovery(std::filesystem::path project_dir);

    /**
     * @brief Discover servers from .mcp.json files.
     * @param existing_names Already-registered server names (skip shadows).
     * @return Vector of discovered server configs.
     * @version 1.8.7
     */
    std::vector<ExternalServerConfig> discover(
        const std::set<std::string>& existing_names) const;

private:
    std::filesystem::path project_dir_;          ///< Project root

    /**
     * @brief Parse a single .mcp.json file.
     * @param path Path to .mcp.json.
     * @param own_socket Own socket path for self-detection.
     * @param existing_names Already-registered server names.
     * @param seen Names already discovered (first file wins).
     * @param out Output vector.
     * @utility
     * @version 1.8.7
     */
    void parse_mcp_json(
        const std::filesystem::path& path,
        const std::filesystem::path& own_socket,
        const std::set<std::string>& existing_names,
        std::set<std::string>& seen,
        std::vector<ExternalServerConfig>& out) const;

    /**
     * @brief Parse one server entry from .mcp.json.
     * @param name Server name.
     * @param cfg JSON object for this server.
     * @param own_socket Own socket path.
     * @param existing_names Already-registered names.
     * @param seen Already-discovered names.
     * @param out Output vector.
     * @utility
     * @version 1.8.7
     */
    void parse_server_entry(
        const std::string& name,
        const nlohmann::json& cfg,
        const std::filesystem::path& own_socket,
        const std::set<std::string>& existing_names,
        std::set<std::string>& seen,
        std::vector<ExternalServerConfig>& out) const;

    /**
     * @brief Infer transport and populate config entry.
     * @param cfg JSON config.
     * @param type Explicit type field (may be empty).
     * @param entry Output config entry.
     * @return true if valid entry.
     * @utility
     * @version 1.8.7
     */
    bool infer_transport(
        const nlohmann::json& cfg,
        const std::string& type,
        ExternalServerConfig& entry) const;

    /**
     * @brief Parse SSE entry fields from JSON config.
     * @param cfg JSON config.
     * @param entry Output config.
     * @return true if valid.
     * @utility
     * @version 1.8.7
     */
    bool parse_sse_entry(
        const nlohmann::json& cfg,
        ExternalServerConfig& entry) const;

    /**
     * @brief Parse stdio entry with env blocklist enforcement.
     * @param cfg JSON config.
     * @param entry Output config.
     * @return true if valid.
     * @utility
     * @version 1.8.7
     */
    bool parse_stdio_entry(
        const nlohmann::json& cfg,
        ExternalServerConfig& entry) const;
};

/**
 * @brief Compute project-unique Unix socket path for self-detection.
 * @param project_dir Absolute path to project directory.
 * @return Socket path: ~/.entropic/socks/{hash8}.sock
 * @version 1.8.7
 */
std::filesystem::path compute_socket_path(
    const std::filesystem::path& project_dir);

/**
 * @brief Environment variable blocklist for .mcp.json env field.
 *
 * These variables cannot be set via .mcp.json for security reasons
 * (CWE-426 — untrusted search path, CWE-94 — code injection).
 *
 * @param key Environment variable name.
 * @return true if the key is blocked.
 * @version 1.8.7
 */
bool is_blocked_env_var(const std::string& key);

} // namespace entropic
