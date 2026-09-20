// SPDX-License-Identifier: Apache-2.0
/**
 * @file entropic_mcp.cpp
 * @brief C API implementation for external MCP server management.
 *
 * Implements entropic_register_mcp_server, entropic_deregister_mcp_server,
 * and entropic_list_mcp_servers from entropic.h.
 *
 * @version 2.0.0
 */

#include "engine_handle.h"

#include <entropic/entropic.h>
#include <entropic/config/loader.h>
#include <entropic/mcp/mcp_json_discovery.h>
#include <entropic/types/logging.h>

#include "json_serializers.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

static auto logger = entropic::log::get("facade.mcp");

/**
 * @brief Check handle prerequisites for MCP server APIs.
 * @param h Engine handle.
 * @return ENTROPIC_OK if valid, error code otherwise.
 * @req REQ-API-005
 * @version 2.0.0
 */
static entropic_error_t check_server_mgr(entropic_handle_t h) {
    if (!h) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    if (!h->server_manager) { return ENTROPIC_ERROR_INVALID_STATE; }
    return ENTROPIC_OK;
}

// ── Named workspaces (gh#166, v2.13.0) ──────────────────────

/**
 * @brief Build a workspace's own server instances rooted at `dir`.
 *
 * The built-in servers take their root at CONSTRUCTION and hold one
 * working directory each, so a second repository needs a second set —
 * this is the whole reason a workspace is an object and not a path.
 * Plugins get the root through `set_working_dir` at load. Weights,
 * tiers and identity are untouched: nothing here reloads a model.
 *
 * @param h Engine handle (configured).
 * @param name Workspace name.
 * @param dir Absolute workspace root.
 * @return Owned workspace with connected servers.
 * @utility
 * @req REQ-MCP-027
 * @version 2.13.0
 */
static std::unique_ptr<EntropicWorkspace> build_workspace(
    entropic_handle_t h, const std::string& name,
    const std::filesystem::path& dir) {
    auto ws = std::make_unique<EntropicWorkspace>();
    ws->name = name;
    ws->root = dir;
    ws->servers = std::make_unique<entropic::ServerManager>(
        h->config.permissions, dir);
    std::vector<std::string> tier_names;
    std::vector<std::string> require_context;
    for (const auto& [tier, cfg] : h->config.models.tiers) {
        if (tier != h->config.models.default_tier) {
            tier_names.push_back(tier);
        }
        if (cfg.requires_context) { require_context.push_back(tier); }
    }
    auto data_dir = entropic::config::resolve_data_dir(h->config);
    ws->servers->init_builtins(h->config.mcp, tier_names,
                               data_dir.string(), require_context);
    ws->servers->load_plugins(h->config.mcp);
    return ws;
}

/**
 * @brief Validate workspace-create arguments and resolve the root.
 *
 * Extracted to keep `entropic_workspace_create` inside the knots returns
 * gate; sets `handle->last_error` with the specific reason.
 *
 * @param handle Engine handle.
 * @param name Requested workspace name.
 * @param dir Requested root directory.
 * @param[out] root Absolute, validated root on success.
 * @return ENTROPIC_OK, or ENTROPIC_ERROR_INVALID_ARGUMENT with a reason.
 * @utility
 * @req REQ-MCP-027
 * @version 2.13.0
 */
static entropic_error_t validate_workspace_args(
    entropic_handle_t handle, const char* name, const char* dir,
    std::filesystem::path& root) {
    if (name == nullptr || *name == '\0' || dir == nullptr
        || *dir == '\0') {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    std::error_code ec;
    root = std::filesystem::absolute(dir, ec);
    std::string err;
    if (ec || !std::filesystem::is_directory(root, ec)) {
        err = std::string("workspace dir is not a directory: ") + dir;
    } else {
        std::lock_guard<std::mutex> guard(handle->workspace_mutex);
        if (handle->workspaces.count(name) > 0) {
            err = std::string("workspace already exists: ") + name;
        }
    }
    if (!err.empty()) {
        handle->last_error = err;
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    return ENTROPIC_OK;
}

/**
 * @brief Create a named workspace (gh#166) — see entropic.h.
 * @param handle Engine handle.
 * @param name Workspace name.
 * @param dir Workspace root directory.
 * @return ENTROPIC_OK, or a typed argument/state error.
 * @req REQ-API-005
 * @req REQ-MCP-027
 * @req REQ-ABI-002
 * @version 2.13.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_workspace_create(entropic_handle_t handle,
                          const char* name,
                          const char* dir) {
    auto rc = check_server_mgr(handle);
    if (rc != ENTROPIC_OK) { return rc; }
    entropic::HandleApiLock lock(handle);
    std::filesystem::path root;
    rc = validate_workspace_args(handle, name, dir, root);
    if (rc == ENTROPIC_OK) {
        try {
            auto ws = build_workspace(handle, name, root);
            ws->servers->initialize();
            std::lock_guard<std::mutex> guard(handle->workspace_mutex);
            handle->workspaces[name] = std::move(ws);
            logger->info("workspace '{}' created at {}", name,
                         root.string());
        } catch (const std::exception& e) {
            // Design rule #5: exceptions never cross the .so boundary.
            handle->last_error = e.what();
            rc = ENTROPIC_ERROR_INTERNAL;
        }
    }
    return rc;
}

/**
 * @brief Bind a session to a workspace (gh#166) — see entropic.h.
 * @param handle Engine handle.
 * @param session_key Session key (NULL or "" = default session).
 * @param name Workspace name.
 * @return ENTROPIC_OK, or a typed argument/state error.
 * @req REQ-API-005
 * @req REQ-MCP-027
 * @req REQ-ABI-002
 * @version 2.13.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_session_bind_workspace(entropic_handle_t handle,
                                const char* session_key,
                                const char* name) {
    auto rc = check_server_mgr(handle);
    if (rc != ENTROPIC_OK) { return rc; }
    if (name == nullptr || *name == '\0' || !handle->engine) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    entropic::HandleApiLock lock(handle);
    std::string key = session_key != nullptr ? session_key : "";
    // Single-exit accumulator (knots returns gate ≤ 3). A conversation
    // cites paths relative to the root it was built in, so re-rooting one
    // mid-life invalidates every citation in it with no error anywhere:
    // refuse rather than silently re-point.
    rc = ENTROPIC_OK;
    if (handle->engine->message_count_for(key) > 0) {
        handle->last_error =
            "session '" + key + "' already holds messages; bind a "
            "workspace before its first turn";
        rc = ENTROPIC_ERROR_INVALID_STATE;
    } else {
        std::lock_guard<std::mutex> guard(handle->workspace_mutex);
        if (handle->workspaces.count(name) == 0) {
            handle->last_error = std::string("unknown workspace: ") + name;
            rc = ENTROPIC_ERROR_INVALID_ARGUMENT;
        } else {
            handle->session_workspace[key] = name;
            logger->info("session '{}' bound to workspace '{}'", key, name);
        }
    }
    return rc;
}

/**
 * @brief Register an external MCP server at runtime.
 *
 * Issue #9 (v2.1.4): parses the FULL ExternalServerConfig from
 * config_json — including `env` and explicit `transport`. Pre-2.1.4
 * the runtime path silently dropped env, leaving spawned children
 * with an empty environment (not even PATH). Env keys are filtered
 * through the same blocklist used by .mcp.json discovery
 * (is_blocked_env_var) so PATH/LD_PRELOAD/etc. cannot be injected.
 *
 * Recognized config_json fields:
 *   - "command" (string)         — stdio executable
 *   - "args"    (string[])       — stdio arguments
 *   - "env"     (object)         — stdio environment (block-filtered)
 *   - "url"     (string)         — SSE endpoint (mutually exclusive)
 *   - "transport" (string)       — "stdio" | "sse"; auto-inferred if absent
 *
 * @param handle Engine handle returned by entropic_create.
 * @param name MCP server name (must be unique).
 * @param config_json JSON-serialized MCP server configuration.
 * @return ENTROPIC_OK or error code.
 * @dg_internal
 * @version 2.1.4
 */
/**
 * @brief Parse an ExternalServerConfig from register-mcp JSON.
 *
 * Extracted from entropic_register_mcp_server to keep it knots-clean.
 * Env vars on the block list are skipped with a warning.
 *
 * @param name Server name.
 * @param j Parsed config JSON.
 * @return Populated ExternalServerConfig.
 * @utility
 * @version 2.3.7
 */
static entropic::ExternalServerConfig parse_external_server_spec(
    const char* name, const nlohmann::json& j) {
    entropic::ExternalServerConfig spec;
    spec.name = name;
    spec.command = j.value("command", "");
    spec.url = j.value("url", "");
    spec.transport = j.value("transport",
        spec.url.empty() ? "stdio" : "sse");
    if (j.contains("args") && j["args"].is_array()) {
        spec.args = j["args"].get<std::vector<std::string>>();
    }
    if (j.contains("env") && j["env"].is_object()) {
        for (auto& [key, val] : j["env"].items()) {
            if (entropic::is_blocked_env_var(key)) {
                logger->warn("register_mcp_server '{}': blocked env var "
                             "'{}' — skipping", name, key);
                continue;
            }
            if (val.is_string()) {
                spec.env[key] = val.get<std::string>();
            }
        }
    }
    return spec;
}

/**
 * @brief Servers a registration targets, and the cwd it spawns in (gh#166).
 *
 * A `"workspace"` field registers the server on that workspace's set and
 * spawns it with cwd = the workspace root — which is what a repo-scoped
 * server like `clew-mcp --repo .` actually needs when one handle serves
 * several repositories.
 *
 * @param handle Engine handle.
 * @param ws_name Workspace name ("" = the handle's default set).
 * @param[in,out] spec Spec whose `working_dir` is filled for a workspace.
 * @return Target manager, or nullptr when the workspace is unknown.
 * @utility
 * @req REQ-MCP-027
 * @version 2.13.0
 */
static entropic::ServerManager* resolve_registration_target(
    entropic_handle_t handle, const std::string& ws_name,
    entropic::ExternalServerConfig& spec) {
    if (ws_name.empty()) { return handle->server_manager.get(); }
    std::lock_guard<std::mutex> guard(handle->workspace_mutex);
    auto it = handle->workspaces.find(ws_name);
    if (it == handle->workspaces.end()) { return nullptr; }
    spec.working_dir = it->second->root.string();
    return it->second->servers.get();
}

/**
 * @brief Register an external MCP server from JSON config (C ABI).
 * @return ENTROPIC_OK on success; the check_server_mgr code for a
 *        bad handle/state, INVALID_ARGUMENT for NULL
 *        name/config_json, CONNECTION_FAILED when the transport
 *        connect throws.
 * @req REQ-MCP-025
 * @req REQ-API-005
 * @req REQ-ABI-002
 * @version 2.13.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_register_mcp_server(
    entropic_handle_t handle,
    const char* name,
    const char* config_json) {

    auto rc = check_server_mgr(handle);
    if (rc != ENTROPIC_OK || !name || !config_json) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    try {
        auto j = nlohmann::json::parse(config_json);
        auto spec = parse_external_server_spec(name, j);
        // gh#166 (v2.13.0): an optional "workspace" field registers the
        // server on that workspace's set instead of the handle's, and
        // spawns it with cwd = the workspace root — which is what a
        // repo-scoped server like `clew-mcp --repo` actually needs.
        auto ws_name = j.value("workspace", std::string{});
        auto* servers = resolve_registration_target(handle, ws_name, spec);
        if (servers == nullptr) {
            handle->last_error = "unknown workspace: " + ws_name;
            rc = ENTROPIC_ERROR_INVALID_ARGUMENT;
        } else {
            servers->connect_external_server(spec);
            logger->info("register_mcp_server: name='{}' env_keys={} "
                         "workspace='{}'", name, spec.env.size(), ws_name);
        }
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        rc = ENTROPIC_ERROR_CONNECTION_FAILED;
    }
    return rc;
}

/**
 * @brief Deregister an external MCP server.
 *
 * @param handle Engine handle returned by entropic_create.
 * @param name MCP server name (must be unique).
 * @return ENTROPIC_OK or error code.
 * @req REQ-MCP-025
 * @req REQ-API-005
 * @req REQ-ABI-002
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_deregister_mcp_server(
    entropic_handle_t handle,
    const char* name) {

    auto rc = check_server_mgr(handle);
    if (rc != ENTROPIC_OK || !name) {
        return rc != ENTROPIC_OK ? rc : ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    try {
        handle->server_manager->disconnect_external_server(name);
        logger->info("deregister_mcp_server: name='{}'", name);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return ENTROPIC_ERROR_SERVER_NOT_FOUND;
    }
}

/**
 * @brief List all MCP servers as JSON array.
 *
 * @param handle Engine handle returned by entropic_create.
 * @return JSON string (caller frees), or NULL.
 * @req REQ-MCP-025
 * @req REQ-API-008
 * @req REQ-API-005
 * @req REQ-ABI-002
 * @version 2.0.0
 */
extern "C" ENTROPIC_EXPORT char*
entropic_list_mcp_servers(entropic_handle_t handle) {
    if (!handle || !handle->configured.load()) {
        return nullptr;
    }
    try {
        nlohmann::json arr = nlohmann::json::array();
        if (handle->server_manager) {
            auto servers = handle->server_manager->list_server_info();
            for (const auto& [name, s] : servers) {
                arr.push_back({{"name", s.name},
                               {"transport", s.transport},
                               {"status", s.status},
                               {"source", s.source}});
            }
        }
        return strdup(arr.dump().c_str());
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return nullptr;
    }
}
