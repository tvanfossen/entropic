// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file mcp_c_api.cpp
 * @brief C API implementation for i_mcp_server.h.
 *
 * Bridges the pure C interface to the C++ MCPServerBase class.
 * Each function casts the opaque handle and delegates.
 *
 * @version 1.8.5
 */

#include <entropic/interfaces/i_mcp_server.h>
#include <entropic/mcp/server_base.h>

#include <cstdlib>
#include <cstring>

/**
 * @brief Cast opaque handle to MCPServerBase pointer.
 * @param server Opaque handle.
 * @return C++ server pointer.
 * @internal
 * @version 1.8.5
 */
static entropic::MCPServerBase* cast(entropic_mcp_server_t server) {
    return reinterpret_cast<entropic::MCPServerBase*>(server);
}

/**
 * @brief Allocate a C string copy for caller-owned return.
 *
 * Uses malloc so the caller can deallocate via entropic_free()
 * (which is the canonical public free in facade/entropic.cpp and
 * wraps free(3)). Mismatched new[]/free is undefined behavior.
 *
 * @param s Source string.
 * @return Heap-allocated copy (free with entropic_free).
 * @internal
 * @version 2.0.5
 */
static char* alloc_string(const std::string& s) {
    auto* p = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

extern "C" {

/**
 * @brief Get server name.
 * @param server Server handle.
 * @return Server name (server-owned).
 * @internal
 * @version 1.8.5
 */
const char* entropic_mcp_server_name(entropic_mcp_server_t server) {
    if (server == nullptr) {
        return "";
    }
    return cast(server)->name().c_str();
}

/**
 * @brief List tools as JSON array.
 * @param server Server handle.
 * @return Caller-owned JSON string.
 * @internal
 * @version 1.8.5
 */
char* entropic_mcp_server_list_tools(entropic_mcp_server_t server) {
    if (server == nullptr) {
        return alloc_string("[]");
    }
    return alloc_string(cast(server)->list_tools());
}

/**
 * @brief Execute a tool.
 * @param server Server handle.
 * @param tool_name Tool name.
 * @param args_json JSON arguments.
 * @return Caller-owned ServerResponse JSON.
 * @internal
 * @version 1.8.5
 */
char* entropic_mcp_server_execute(
    entropic_mcp_server_t server,
    const char* tool_name,
    const char* args_json) {
    if (server == nullptr || tool_name == nullptr) {
        return alloc_string(
            R"({"result":"Error: null server or tool_name","directives":[]})");
    }
    auto result = cast(server)->execute(
        tool_name, args_json ? args_json : "{}");
    return alloc_string(result);
}

/**
 * @brief Configure a server.
 * @param server Server handle.
 * @param config_json Configuration JSON.
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 1.8.5
 */
entropic_error_t entropic_mcp_server_configure(
    entropic_mcp_server_t server,
    const char* config_json) {
    if (server == nullptr) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    bool ok = cast(server)->configure(
        config_json ? config_json : "{}");
    return ok ? ENTROPIC_OK : ENTROPIC_ERROR_INVALID_CONFIG;
}

/**
 * @brief Set server working directory.
 * @param server Server handle.
 * @param path Working directory path.
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 1.8.5
 */
entropic_error_t entropic_mcp_server_set_working_dir(
    entropic_mcp_server_t server,
    const char* path) {
    if (server == nullptr || path == nullptr) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    bool ok = cast(server)->set_working_dir(path);
    return ok ? ENTROPIC_OK : ENTROPIC_ERROR_IO;
}

/**
 * @brief Destroy a server instance.
 * @param server Server handle.
 * @internal
 * @version 1.8.5
 */
void entropic_mcp_server_destroy(entropic_mcp_server_t server) {
    delete cast(server);
}

} // extern "C"
