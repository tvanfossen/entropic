/**
 * @file mcp_json_discovery.cpp
 * @brief MCPJsonDiscovery implementation.
 * @version 1.8.7
 */

#include <entropic/mcp/mcp_json_discovery.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <sstream>
#include <iomanip>

static auto logger = entropic::log::get("mcp.discovery");

namespace entropic {

/**
 * @brief Construct with project directory.
 * @param project_dir Project root.
 * @internal
 * @version 1.8.7
 */
MCPJsonDiscovery::MCPJsonDiscovery(std::filesystem::path project_dir)
    : project_dir_(std::move(project_dir)) {}

/**
 * @brief Discover servers from .mcp.json files.
 * @param existing_names Already-registered server names.
 * @return Vector of discovered configs.
 * @internal
 * @version 1.8.7
 */
std::vector<ExternalServerConfig> MCPJsonDiscovery::discover(
    const std::set<std::string>& existing_names) const {

    auto own_socket = compute_socket_path(project_dir_);
    std::vector<ExternalServerConfig> result;
    std::set<std::string> seen;

    // Project-level first, then global fallback
    auto project_mcp = project_dir_ / ".mcp.json";
    auto home = std::filesystem::path(getenv("HOME") ? getenv("HOME") : "");
    auto global_mcp = home / ".entropic" / ".mcp.json";

    parse_mcp_json(project_mcp, own_socket, existing_names,
                   seen, result);
    parse_mcp_json(global_mcp, own_socket, existing_names,
                   seen, result);

    logger->info("Discovered {} external servers from .mcp.json",
                 result.size());
    return result;
}

/**
 * @brief Parse a single .mcp.json file.
 * @param path Path to .mcp.json.
 * @param own_socket Own socket path.
 * @param existing_names Already-registered names.
 * @param seen Names discovered so far.
 * @param out Output vector.
 * @utility
 * @version 1.8.7
 */
void MCPJsonDiscovery::parse_mcp_json(
    const std::filesystem::path& path,
    const std::filesystem::path& own_socket,
    const std::set<std::string>& existing_names,
    std::set<std::string>& seen,
    std::vector<ExternalServerConfig>& out) const {

    if (!std::filesystem::exists(path)) {
        return;
    }

    nlohmann::json data;
    try {
        std::ifstream f(path);
        data = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        logger->warn("Failed to read {}: {}", path.string(), e.what());
        return;
    }

    auto servers_it = data.find("mcpServers");
    if (servers_it == data.end() || !servers_it->is_object()) {
        return;
    }

    for (auto& [name, cfg] : servers_it->items()) {
        parse_server_entry(name, cfg, own_socket,
                           existing_names, seen, out);
    }
}

/**
 * @brief Parse one server entry from .mcp.json.
 * @param name Server name.
 * @param cfg JSON config object.
 * @param own_socket Own socket path for self-detection.
 * @param existing_names Already-registered names.
 * @param seen Already-discovered names in this run.
 * @param out Output vector.
 * @utility
 * @version 1.8.7
 */
void MCPJsonDiscovery::parse_server_entry(
    const std::string& name,
    const nlohmann::json& cfg,
    const std::filesystem::path& own_socket,
    const std::set<std::string>& existing_names,
    std::set<std::string>& seen,
    std::vector<ExternalServerConfig>& out) const {

    // Skip if already discovered from a higher-priority file
    if (seen.count(name) > 0) {
        return;
    }

    // Self-detection: skip own socket
    if (cfg.contains("path")) {
        auto sock = std::filesystem::path(
            cfg["path"].get<std::string>());
        if (std::filesystem::weakly_canonical(sock) ==
            std::filesystem::weakly_canonical(own_socket)) {
            logger->debug("Skipping '{}' (matches own socket)", name);
            seen.insert(name);
            return;
        }
    }

    // Shadow warning: skip if already registered
    if (existing_names.count(name) > 0) {
        logger->warn(".mcp.json entry '{}' shadowed by existing "
                     "config — skipping", name);
        seen.insert(name);
        return;
    }

    // Determine transport
    ExternalServerConfig entry;
    entry.name = name;
    std::string type = cfg.value("type", "");

    if (infer_transport(cfg, type, entry)) {
        seen.insert(name);
        out.push_back(std::move(entry));
        logger->info("Discovered external server '{}' "
                     "(transport={})", name, entry.transport);
    }
}

/**
 * @brief Infer transport and populate config entry.
 * @param cfg JSON config.
 * @param type Explicit type field (may be empty).
 * @param entry Output config entry.
 * @return true if valid entry.
 * @utility
 * @version 1.8.7
 */
bool MCPJsonDiscovery::infer_transport(
    const nlohmann::json& cfg,
    const std::string& type,
    ExternalServerConfig& entry) const {

    bool has_url = cfg.contains("url");
    bool has_command = cfg.contains("command");

    // Explicit type overrides inference
    if (type == "sse" || type == "http" ||
        (type.empty() && has_url)) {
        return parse_sse_entry(cfg, entry);
    }
    if (type == "stdio" || (type.empty() && has_command)) {
        return parse_stdio_entry(cfg, entry);
    }

    logger->warn(".mcp.json entry '{}': cannot determine transport "
                 "(no url or command)", entry.name);
    return false;
}

/**
 * @brief Parse SSE entry fields.
 * @param cfg JSON config.
 * @param entry Output config.
 * @return true if valid.
 * @utility
 * @version 1.8.7
 */
bool MCPJsonDiscovery::parse_sse_entry(
    const nlohmann::json& cfg,
    ExternalServerConfig& entry) const {

    entry.url = cfg.value("url", "");
    if (entry.url.empty()) {
        logger->warn(".mcp.json entry '{}' missing 'url' — skipping",
                     entry.name);
        return false;
    }
    entry.transport = "sse";
    return true;
}

/**
 * @brief Parse stdio entry fields with env blocklist enforcement.
 * @param cfg JSON config.
 * @param entry Output config.
 * @return true if valid.
 * @utility
 * @version 1.8.7
 */
bool MCPJsonDiscovery::parse_stdio_entry(
    const nlohmann::json& cfg,
    ExternalServerConfig& entry) const {

    entry.command = cfg.value("command", "");
    if (entry.command.empty()) {
        logger->warn(".mcp.json entry '{}' missing 'command' — "
                     "skipping", entry.name);
        return false;
    }
    entry.transport = "stdio";

    // Args
    if (cfg.contains("args") && cfg["args"].is_array()) {
        for (const auto& arg : cfg["args"]) {
            entry.args.push_back(arg.get<std::string>());
        }
    }

    // Env with blocklist enforcement
    if (cfg.contains("env") && cfg["env"].is_object()) {
        for (auto& [key, val] : cfg["env"].items()) {
            if (is_blocked_env_var(key)) {
                logger->warn(".mcp.json '{}': blocked env var '{}' "
                             "— skipping", entry.name, key);
                continue;
            }
            entry.env[key] = val.get<std::string>();
        }
    }
    return true;
}

/**
 * @brief Compute project-unique Unix socket path.
 * @param project_dir Project directory.
 * @return ~/.entropic/socks/{hash8}.sock
 * @utility
 * @version 1.8.7
 */
std::filesystem::path compute_socket_path(
    const std::filesystem::path& project_dir) {

    auto resolved = std::filesystem::weakly_canonical(project_dir);
    auto input = resolved.string();

    // SHA-256 first 8 hex chars (simplified: use std::hash)
    auto h = std::hash<std::string>{}(input);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << h;
    auto hash_str = ss.str().substr(0, 8);

    auto home = std::filesystem::path(
        getenv("HOME") ? getenv("HOME") : "/tmp");
    return home / ".entropic" / "socks" / (hash_str + ".sock");
}

/**
 * @brief Check if key matches a blocked prefix pattern.
 * @param key Variable name.
 * @return true if key starts with a blocked prefix.
 * @utility
 * @version 1.8.8
 */
static bool has_blocked_prefix(const std::string& key) {
    return (key.size() >= 9 && key.substr(0, 9) == "ENTROPIC_") ||
           (key.size() >= 5 && key.substr(0, 5) == "DYLD_");
}

/**
 * @brief Check if an environment variable is blocked.
 * @param key Variable name.
 * @return true if blocked.
 * @utility
 * @version 1.8.7
 */
bool is_blocked_env_var(const std::string& key) {
    static const std::array<std::string, 7> blocked = {{
        "LD_PRELOAD", "LD_LIBRARY_PATH", "PATH",
        "HOME", "SHELL", "DYLD_INSERT_LIBRARIES",
        "DYLD_LIBRARY_PATH"
    }};

    bool exact_match = std::any_of(blocked.begin(), blocked.end(),
        [&key](const std::string& b) { return key == b; });

    return exact_match || has_blocked_prefix(key);
}

} // namespace entropic
