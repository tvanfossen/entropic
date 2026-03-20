/**
 * @file entropic_mcp.cpp
 * @brief C API implementation for external MCP server management.
 *
 * Implements entropic_register_mcp_server, entropic_deregister_mcp_server,
 * and entropic_list_mcp_servers from entropic.h.
 *
 * @version 1.8.7
 */

#include <entropic/entropic.h>
#include <entropic/mcp/server_manager.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <cstring>

static auto logger = entropic::log::get("facade.mcp");

// Forward declaration — engine internals not visible to this file.
// The actual implementation retrieves ServerManager from the handle.
// For now, these are stubs that will be wired when the facade
// owns the full engine state (currently entropic.cpp manages lifecycle).

/**
 * @brief Register an external MCP server at runtime.
 * @param handle Engine handle.
 * @param name Server name.
 * @param config_json JSON config.
 * @return Error code.
 * @internal
 * @version 1.8.7
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_register_mcp_server(
    entropic_handle_t handle,
    const char* name,
    const char* config_json) {

    if (!handle || !name || !config_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    // TODO: Wire to handle->server_manager when facade owns engine state
    // For now, log and return stub
    logger->info("entropic_register_mcp_server: name='{}' config='{}'",
                 name, config_json);
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Deregister an external MCP server.
 * @param handle Engine handle.
 * @param name Server name.
 * @return Error code.
 * @internal
 * @version 1.8.7
 */
extern "C" ENTROPIC_EXPORT entropic_error_t
entropic_deregister_mcp_server(
    entropic_handle_t handle,
    const char* name) {

    if (!handle || !name) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    logger->info("entropic_deregister_mcp_server: name='{}'", name);
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief List all MCP servers as JSON.
 * @param handle Engine handle.
 * @return JSON string (caller-owned), or NULL.
 * @internal
 * @version 1.8.7
 */
extern "C" ENTROPIC_EXPORT char*
entropic_list_mcp_servers(entropic_handle_t handle) {
    if (!handle) {
        return nullptr;
    }

    logger->info("entropic_list_mcp_servers called");
    // Stub: return empty servers object
    const char* stub = R"({"servers":{}})";
    auto len = std::strlen(stub) + 1;
    auto* buf = static_cast<char*>(entropic_alloc(len));
    if (buf) {
        std::memcpy(buf, stub, len);
    }
    return buf;
}
