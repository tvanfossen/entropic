// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_plugin.cpp
 * @brief gh#133: a conformant third-party MCP server plugin, built as a
 *        loadable module for the plugin-loader tests.
 *
 * Implements the full 9-entry-point contract from
 * `entropic/interfaces/i_mcp_server.h` exactly as an external consumer
 * would — this file deliberately uses ONLY that public header, no engine
 * internals, so it is a faithful stand-in for a real third-party plugin.
 *
 * @par Why the entry points below carry no visibility attribute
 * They are defined bare — plain `extern "C"`, exactly as one implements a C
 * API — and inherit their visibility solely from the declarations in
 * i_mcp_server.h. The CMake target then builds this with
 * `-fvisibility=hidden`, which is what a real plugin project does.
 *
 * That combination makes this fixture the control for the gh#133 header
 * fix. Measured on GCC 11.4 (`-fvisibility=hidden`, exported symbol count):
 *
 *   definition     header declaration    exported
 *   ------------   -------------------   --------
 *   bare           bare                  0
 *   bare           ENTROPIC_EXPORT       all
 *   own attribute  bare                  all
 *
 * So a plugin author who writes the obvious bare definition gets a `.so`
 * that exports NOTHING until the declarations carry ENTROPIC_EXPORT — every
 * dlsym in the loader returns null. Writing the attribute on the definitions
 * here instead (row 3) would export either way and silently void this
 * control, which is why it is deliberately omitted.
 *
 * Note this is NOT the mechanism originally reported by the consumer, which
 * claimed GCC warns and ignores a definition-site attribute; that does not
 * reproduce on GCC 11.4 (row 3). The header fix matters for the reason in
 * row 1, not that one.
 *
 * Exposes one tool, `echo`, which returns its `text` argument.
 *
 * @version 2.10.1
 */

#include <entropic/interfaces/i_mcp_server.h>

#include <cstdlib>
#include <cstring>
#include <string>

// The CMake target builds this source twice: once as the conformant plugin,
// and once with TEST_PLUGIN_API_VERSION overridden to an incompatible value
// so the loader's version-mismatch rejection has a real .so to reject.
#ifndef TEST_PLUGIN_API_VERSION
#define TEST_PLUGIN_API_VERSION ENTROPIC_MCP_PLUGIN_API_VERSION
#endif

#ifndef TEST_PLUGIN_SERVER_NAME
#define TEST_PLUGIN_SERVER_NAME "testplugin"
#endif

namespace {

/**
 * @brief Concrete server instance behind the opaque handle.
 * @version 2.10.1
 */
struct TestPluginServer {
    std::string working_dir;  ///< Last value from set_working_dir
    std::string config_json;  ///< Last value from configure
};

/**
 * @brief Duplicate a std::string into a malloc'd C string.
 *
 * The contract requires strings returned by list_tools/execute to be
 * caller-owned and freed via entropic_free(), so they must come from
 * malloc — not new — to match this plugin's entropic_free().
 *
 * @param s Source string.
 * @return malloc'd copy, or nullptr on allocation failure.
 * @version 2.10.1
 */
char* dup_c(const std::string& s) {
    auto* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

/**
 * @brief Cast an opaque handle back to the concrete instance.
 * @param server Opaque handle.
 * @return Concrete pointer (may be nullptr).
 * @version 2.10.1
 */
TestPluginServer* self(entropic_mcp_server_t server) {
    return reinterpret_cast<TestPluginServer*>(server);
}

}  // namespace

extern "C" {

/**
 * @brief Report the plugin API version this plugin was built against.
 * @return TEST_PLUGIN_API_VERSION (the contract version unless the build
 *         overrode it to produce a deliberately-incompatible plugin).
 * @version 2.10.1
 */
int entropic_plugin_api_version() {
    return TEST_PLUGIN_API_VERSION;
}

/**
 * @brief Construct a server instance.
 * @return Opaque handle, owned by the caller until destroy.
 * @version 2.10.1
 */
entropic_mcp_server_t entropic_create_server() {
    return reinterpret_cast<entropic_mcp_server_t>(new TestPluginServer());
}

/**
 * @brief Get the server name (used as the tool-routing prefix).
 * @param server Server handle (unused — name is constant).
 * @return TEST_PLUGIN_SERVER_NAME, valid for the process lifetime.
 * @version 2.10.1
 */
const char* entropic_mcp_server_name(
    entropic_mcp_server_t server) {
    (void)server;
    return TEST_PLUGIN_SERVER_NAME;
}

/**
 * @brief List this plugin's tools.
 * @param server Server handle (unused — tool set is static).
 * @return malloc'd JSON array; caller frees with entropic_free().
 * @version 2.10.1
 */
char* entropic_mcp_server_list_tools(
    entropic_mcp_server_t server) {
    (void)server;
    // inputSchema is camelCase per the contract (matches data/tools/*/*.json).
    return dup_c(
        R"([{"name":"echo","description":"Echo the text argument back",)"
        R"("inputSchema":{"type":"object","properties":{"text":)"
        R"({"type":"string"}},"required":["text"]}}])");
}

/**
 * @brief Execute a tool.
 * @param server Server handle.
 * @param tool_name Local tool name (no server prefix).
 * @param args_json JSON arguments.
 * @return malloc'd ServerResponse envelope; caller frees with entropic_free().
 * @version 2.10.1
 *
 * Hand-rolls the `text` extraction rather than pulling in a JSON library —
 * a real plugin would use its own; the point here is that the engine never
 * sees the plugin's dependencies.
 */
char* entropic_mcp_server_execute(
    entropic_mcp_server_t server, const char* tool_name,
    const char* args_json) {
    (void)server;
    if (tool_name == nullptr || std::strcmp(tool_name, "echo") != 0) {
        return dup_c(
            R"({"result":"Error: unknown tool","directives":[]})");
    }

    std::string args = (args_json != nullptr) ? args_json : "";
    std::string text;
    auto key = args.find("\"text\"");
    if (key != std::string::npos) {
        auto open = args.find('"', args.find(':', key) + 1);
        auto close = (open == std::string::npos)
                         ? std::string::npos
                         : args.find('"', open + 1);
        if (close != std::string::npos) {
            text = args.substr(open + 1, close - open - 1);
        }
    }

    return dup_c(R"({"result":"echo: )" + text + R"(","directives":[]})");
}

/**
 * @brief Configure the instance after creation.
 * @param server Server handle.
 * @param config_json JSON configuration.
 * @return ENTROPIC_OK, or ENTROPIC_ERROR_INVALID_CONFIG on a null handle.
 * @version 2.10.1
 */
entropic_error_t entropic_mcp_server_configure(
    entropic_mcp_server_t server, const char* config_json) {
    if (server == nullptr) {
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }
    self(server)->config_json = (config_json != nullptr) ? config_json : "";
    return ENTROPIC_OK;
}

/**
 * @brief Set the working directory.
 * @param server Server handle.
 * @param path Working directory.
 * @return ENTROPIC_OK, or ENTROPIC_ERROR_INVALID_CONFIG on a null handle.
 * @version 2.10.1
 */
entropic_error_t entropic_mcp_server_set_working_dir(
    entropic_mcp_server_t server, const char* path) {
    if (server == nullptr) {
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }
    self(server)->working_dir = (path != nullptr) ? path : "";
    return ENTROPIC_OK;
}

/**
 * @brief Destroy a server instance.
 * @param server Handle to destroy; NULL is a safe no-op.
 * @version 2.10.1
 */
void entropic_mcp_server_destroy(
    entropic_mcp_server_t server) {
    delete self(server);
}

/**
 * @brief Free a string returned by list_tools or execute.
 * @param ptr Pointer to free; NULL is a safe no-op.
 * @version 2.10.1
 */
void entropic_free(void* ptr) {
    std::free(ptr);
}

}  // extern "C"
