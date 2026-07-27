// SPDX-License-Identifier: Apache-2.0
/**
 * @file plugin_server.h
 * @brief dlopen-loaded in-process MCP server plugin (gh#133).
 *
 * Owns one plugin `.so`: the dlopen handle, the resolved C entry points from
 * `entropic/interfaces/i_mcp_server.h`, and the server instance created
 * through them. Destruction destroys the instance and closes the library, in
 * that order.
 *
 * @par Why this is not an MCPServerBase subclass
 * MCPServerBase::list_tools() and ::execute() are non-virtual — they iterate
 * the server's own ToolRegistry — so an adapter deriving from it could not
 * intercept them, and ServerManager would see an empty registry instead of
 * the plugin's tools. Making them virtual would change the MCPServerBase
 * vtable, which `i_mcp_server.h` defines as requiring a plugin-API version
 * bump, and would break any consumer already subclassing the exported base.
 *
 * Instead this mirrors the established ExternalMCPClient pattern: external
 * servers are likewise not MCPServerBase subclasses but a parallel map with
 * their own branch in ServerManager::route_tool_call. Plugins are the third
 * such peer.
 *
 * @version 2.10.1
 */

#pragma once

#include <entropic/interfaces/i_mcp_server.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace entropic {

/**
 * @brief A loaded MCP server plugin and its C-ABI entry points.
 *
 * Non-copyable and non-movable: it owns a raw dlopen handle and a plugin
 * instance whose lifetime is tied to that library staying mapped.
 *
 * @version 2.10.1
 */
class PluginServer {
public:
    /**
     * @brief Load a plugin `.so` and construct its server instance.
     *
     * Performs the full sequence: dlopen, resolve all nine entry points,
     * verify entropic_plugin_api_version() against
     * ENTROPIC_MCP_PLUGIN_API_VERSION, then entropic_create_server().
     * Every failure is logged with the offending path before returning.
     *
     * @param path Filesystem path to the plugin shared object.
     * @param[out] out Receives the loaded plugin on success; untouched
     *             otherwise.
     * @return ENTROPIC_OK, ENTROPIC_ERROR_PLUGIN_LOAD_FAILED (dlopen or a
     *         missing entry point or a null instance), or
     *         ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH.
     * @version 2.10.1
     */
    static entropic_error_t load(const std::filesystem::path& path,
                                 std::unique_ptr<PluginServer>& out);

    /**
     * @brief Destroy the plugin instance, then close the library.
     * @version 2.10.1
     */
    ~PluginServer();

    PluginServer(const PluginServer&) = delete;
    PluginServer& operator=(const PluginServer&) = delete;
    PluginServer(PluginServer&&) = delete;
    PluginServer& operator=(PluginServer&&) = delete;

    /**
     * @brief Server name, used as the tool-routing prefix.
     * @return Name reported by entropic_mcp_server_name() at load time.
     * @utility
     * @version 2.10.1
     */
    const std::string& name() const { return name_; }

    /**
     * @brief Path this plugin was loaded from.
     * @return Filesystem path.
     * @utility
     * @version 2.10.1
     */
    const std::filesystem::path& path() const { return path_; }

    /**
     * @brief List the plugin's tools.
     * @return JSON array string; "[]" if the plugin returned nothing.
     * @version 2.10.1
     */
    std::string list_tools() const;

    /**
     * @brief Execute one of the plugin's tools.
     * @param tool_name Local tool name (no server prefix).
     * @param args_json JSON arguments.
     * @return ServerResponse JSON envelope.
     * @version 2.10.1
     */
    std::string execute(const std::string& tool_name,
                        const std::string& args_json);

    /**
     * @brief Pass configuration to the plugin instance.
     * @param config_json JSON configuration string.
     * @return Whatever the plugin's configure entry point returns.
     * @version 2.10.1
     */
    entropic_error_t configure(const std::string& config_json);

    /**
     * @brief Set the plugin's working directory.
     * @param dir Directory path.
     * @return Whatever the plugin's set_working_dir entry point returns.
     * @version 2.10.1
     */
    entropic_error_t set_working_dir(const std::string& dir);

private:
    /* Signatures of the plugin-side entry points (see i_mcp_server.h). */
    using version_fn_t = int (*)();
    using create_fn_t = entropic_mcp_server_t (*)();
    using name_fn_t = const char* (*)(entropic_mcp_server_t);
    using list_tools_fn_t = char* (*)(entropic_mcp_server_t);
    using execute_fn_t = char* (*)(entropic_mcp_server_t, const char*,
                                   const char*);
    using configure_fn_t = entropic_error_t (*)(entropic_mcp_server_t,
                                                const char*);
    using set_dir_fn_t = entropic_error_t (*)(entropic_mcp_server_t,
                                              const char*);
    using destroy_fn_t = void (*)(entropic_mcp_server_t);
    using free_fn_t = void (*)(void*);

    /**
     * @brief Construct around an open library handle.
     * @param handle dlopen result (ownership transferred).
     * @param path Path the handle was opened from.
     * @version 2.10.1
     */
    PluginServer(void* handle, std::filesystem::path path);

    /**
     * @brief Resolve entry points, check version, create the instance.
     * @return ENTROPIC_OK or the typed failure.
     * @utility
     * @version 2.10.1
     */
    entropic_error_t resolve_and_init();

    /**
     * @brief Resolve every entry point and verify the reported API version.
     * @return ENTROPIC_OK or the typed failure.
     * @utility
     * @version 2.10.1
     */
    entropic_error_t resolve_and_check_version();

    /**
     * @brief Create the plugin's server instance and read its name.
     * @return ENTROPIC_OK or ENTROPIC_ERROR_PLUGIN_LOAD_FAILED.
     * @utility
     * @version 2.10.1
     */
    entropic_error_t create_instance();

    /**
     * @brief dlsym all nine entry points, logging each one that is missing.
     * @return true when all resolved.
     * @utility
     * @version 2.10.1
     */
    bool resolve_symbols();

    /**
     * @brief dlsym one entry point, logging and flagging when it is absent.
     * @param symbol_name Symbol to resolve.
     * @param[in,out] ok Cleared to false when the symbol is missing.
     * @return Symbol address, or nullptr.
     * @utility
     * @version 2.10.1
     */
    void* resolve_one(const char* symbol_name, bool& ok) const;

    /**
     * @brief Resolve the process-level entry points.
     * @return true when all resolved.
     * @utility
     * @version 2.10.1
     */
    bool resolve_factory_symbols();

    /**
     * @brief Resolve the per-instance entry points.
     * @return true when all resolved.
     * @utility
     * @version 2.10.1
     */
    bool resolve_instance_symbols();

    /**
     * @brief Copy a plugin-allocated string and release it via the plugin.
     *
     * Strings crossing back from the plugin are owned by the caller and must
     * be freed with THAT plugin's entropic_free — not the engine's allocator
     * — so this pairs the copy and the free in one place.
     *
     * @param raw Plugin-allocated string; may be null.
     * @return Owned copy, or empty string when raw is null.
     * @utility
     * @version 2.10.1
     */
    std::string take_string(char* raw) const;

    void* handle_{nullptr};                  ///< dlopen handle
    std::filesystem::path path_;             ///< Source .so path
    std::string name_;                       ///< Routing prefix
    entropic_mcp_server_t instance_{nullptr};///< Plugin server instance

    version_fn_t version_fn_{nullptr};       ///< entropic_plugin_api_version
    create_fn_t create_fn_{nullptr};         ///< entropic_create_server
    name_fn_t name_fn_{nullptr};             ///< entropic_mcp_server_name
    list_tools_fn_t list_tools_fn_{nullptr}; ///< entropic_mcp_server_list_tools
    execute_fn_t execute_fn_{nullptr};       ///< entropic_mcp_server_execute
    configure_fn_t configure_fn_{nullptr};   ///< entropic_mcp_server_configure
    set_dir_fn_t set_dir_fn_{nullptr};       ///< ..._set_working_dir
    destroy_fn_t destroy_fn_{nullptr};       ///< entropic_mcp_server_destroy
    free_fn_t free_fn_{nullptr};             ///< entropic_free

    /**
     * @brief Serialises calls into instance_.
     *
     * i_mcp_server.h promises plugins that the engine never runs execute(),
     * configure(), or set_working_dir() concurrently on one handle, so a
     * plugin author may keep unguarded instance state. Holding the lock here
     * makes that guarantee true by construction rather than depending on
     * every caller upstream behaving.
     */
    mutable std::mutex call_mutex_;
};

} // namespace entropic
