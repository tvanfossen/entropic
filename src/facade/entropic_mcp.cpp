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
#include <entropic/types/logging.h>

#include "json_serializers.h"

#include <cstring>

static auto logger = entropic::log::get("facade.mcp");

/**
 * @brief Check handle prerequisites for MCP server APIs.
 * @param h Engine handle.
 * @return ENTROPIC_OK if valid, error code otherwise.
 * @internal
 * @version 2.0.0
 */
static entropic_error_t check_server_mgr(entropic_handle_t h) {
    if (!h) { return ENTROPIC_ERROR_INVALID_HANDLE; }
    if (!h->server_manager) { return ENTROPIC_ERROR_INVALID_STATE; }
    return ENTROPIC_OK;
}

/**
 * @brief Register an external MCP server at runtime.
 *
 * Parses config_json for "command", "args", and "url" fields
 * to determine transport type (stdio or SSE).
 *
 * @param handle Engine handle returned by entropic_create.
 * @param name MCP server name (must be unique).
 * @param config_json JSON-serialized MCP server configuration.
 * @return ENTROPIC_OK or error code.
 * @internal
 * @version 2.0.0
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
        auto cmd = j.value("command", "");
        auto url = j.value("url", "");
        std::vector<std::string> args;
        if (j.contains("args") && j["args"].is_array()) {
            args = j["args"].get<std::vector<std::string>>();
        }
        handle->server_manager->connect_external_server(
            name, cmd, args, url);
        logger->info("register_mcp_server: name='{}'", name);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        handle->last_error = e.what();
        return ENTROPIC_ERROR_CONNECTION_FAILED;
    }
}

/**
 * @brief Deregister an external MCP server.
 *
 * @param handle Engine handle returned by entropic_create.
 * @param name MCP server name (must be unique).
 * @return ENTROPIC_OK or error code.
 * @internal
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
 * @internal
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
