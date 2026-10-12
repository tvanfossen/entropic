// SPDX-License-Identifier: Apache-2.0
/**
 * @file discovery_policy.h
 * @brief Which external MCP servers a server set may launch (v2.14.0).
 *
 * Pure decisions, kept out of ServerManager so the CPU suite can pin them
 * without spawning anything.
 *
 * @par The Claude-Code model (gh#199)
 * A `.mcp.json` the engine discovers is a list of PROCESSES a repository asks
 * the host to start. Before v2.14.0 a named workspace started all of them
 * unconditionally — a consumer's handle, configured by file precisely to avoid
 * project config, launched a 58-tool application server and four nested
 * launchers. Now a discovered entry connects only when enabled: refused when
 * named in `mcp.disabled_mcpjson_servers`; connected when named in
 * `mcp.enabled_mcpjson_servers` or `mcp.enable_all_project_mcp_servers` is
 * set; otherwise the `ENTROPIC_HOOK_PRE_MCP_SERVER_CONNECT` pre-hook decides,
 * and with nobody to ask it is not connected.
 *
 * @par Self-connection (gh#200)
 * The old guard compared a non-standard `"path"` key against a socket derived
 * from the ServerManager root — it could never match a stdio entry, and our own
 * tracked `.mcp.json` (`entropic mcp-bridge`) escaped it only because its NAME
 * collided with the built-in `entropic` server. `launches_own_bridge` resolves
 * where an `entropic mcp-bridge` entry would actually connect, exactly as
 * `src/cli/mcp_bridge.cpp` does, and compares that with the socket this handle
 * really binds.
 *
 * @version 2.14.0
 */
#pragma once

#include <entropic/mcp/mcp_json_discovery.h>
#include <entropic/types/config.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace entropic {

/**
 * @brief What configuration alone says about a discovered server.
 * @version 2.14.0
 */
enum class McpJsonConfigVerdict {
    disabled,  ///< Named in disabled_mcpjson_servers — never connect.
    enabled,   ///< Named in enabled_mcpjson_servers, or enable-all is set.
    ask        ///< Config is silent; the pre-connect hook decides.
};

/**
 * @brief Config's verdict on one discovered server. Disabled beats enabled.
 * @param name Server name from `.mcp.json`.
 * @param cfg The set's MCP config.
 * @return disabled, enabled, or ask.
 * @utility
 * @version 2.14.0
 */
inline McpJsonConfigVerdict mcpjson_config_verdict(const std::string& name,
                                                   const MCPConfig& cfg) {
    auto named = [&name](const std::vector<std::string>& list) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    if (named(cfg.disabled_mcpjson_servers)) {
        return McpJsonConfigVerdict::disabled;
    }
    const bool on = cfg.enable_all_project_mcp_servers
                 || named(cfg.enabled_mcpjson_servers);
    return on ? McpJsonConfigVerdict::enabled : McpJsonConfigVerdict::ask;
}

/**
 * @brief The value following `flag` in `args`, or "".
 * @param args Command arguments.
 * @param flag Flag to look for (e.g. "--socket").
 * @return The next argument, or "" when absent.
 * @utility
 * @version 2.14.0
 */
inline std::string arg_after(const std::vector<std::string>& args,
                             const std::string& flag) {
    auto it = std::find(args.begin(), args.end(), flag);
    return (it == args.end() || it + 1 == args.end()) ? std::string{} : *(it + 1);
}

/**
 * @brief Whether a stdio entry runs `entropic mcp-bridge`.
 * @param cfg Server config.
 * @return true for the engine's own bridge relay.
 * @utility
 * @version 2.14.0
 */
inline bool is_entropic_bridge(const ExternalServerConfig& cfg) {
    const bool cmd = std::filesystem::path(cfg.command).filename() == "entropic";
    const bool sub = std::find(cfg.args.begin(), cfg.args.end(), "mcp-bridge")
                     != cfg.args.end();
    return cfg.transport == "stdio" && cmd && sub;
}

/**
 * @brief Whether launching `cfg` would connect this handle to itself.
 *
 * Mirrors `entropic mcp-bridge`'s own resolution: `--socket` wins, else the
 * socket derived from `--project-dir`, else from the cwd the child is given
 * (`cfg.working_dir`, which ServerManager fills with its root).
 *
 * @param cfg Server config.
 * @param own_socket The socket this handle's bridge binds; empty = no bridge.
 * @param spawn_root Root the child would run in when cfg.working_dir is empty.
 * @return true when it is our own bridge.
 * @utility
 * @version 2.14.0
 */
inline bool launches_own_bridge(const ExternalServerConfig& cfg,
                                const std::filesystem::path& own_socket,
                                const std::filesystem::path& spawn_root) {
    namespace fs = std::filesystem;
    bool self = false;
    if (!own_socket.empty() && is_entropic_bridge(cfg)) {
        const auto sock = arg_after(cfg.args, "--socket");
        auto dir = arg_after(cfg.args, "--project-dir");
        if (dir.empty()) {
            dir = cfg.working_dir.empty() ? spawn_root.string() : cfg.working_dir;
        }
        const fs::path target = !sock.empty()
            ? fs::path(sock) : compute_socket_path(fs::weakly_canonical(dir));
        self = fs::weakly_canonical(target) == fs::weakly_canonical(own_socket);
    }
    return self;
}

/**
 * @brief The pre-connect hook's context JSON for one discovered server.
 * @param cfg Server config.
 * @param root Root the server would run in.
 * @return JSON string (see ENTROPIC_HOOK_PRE_MCP_SERVER_CONNECT).
 * @utility
 * @version 2.14.0
 */
inline std::string discovered_server_json(const ExternalServerConfig& cfg,
                                          const std::filesystem::path& root) {
    nlohmann::json j;
    j["name"] = cfg.name;
    j["transport"] = cfg.transport;
    j["command"] = cfg.command;
    j["args"] = cfg.args;
    j["url"] = cfg.url;
    j["root"] = cfg.working_dir.empty() ? root.string() : cfg.working_dir;
    return j.dump();
}

}  // namespace entropic
