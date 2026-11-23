// SPDX-License-Identifier: Apache-2.0
/**
 * @file plugin_server.cpp
 * @brief dlopen-loaded MCP server plugin implementation (gh#133).
 *
 * @version 2.10.1
 */

#include <entropic/mcp/plugin_server.h>
#include <entropic/types/logging.h>

#include <dlfcn.h>

#include <utility>

static auto logger = entropic::log::get("mcp.plugin");

namespace entropic {

/**
 * @brief Construct around an open library handle.
 * @param handle dlopen result (ownership transferred).
 * @param path Path the handle was opened from.
 * @utility
 * @version 2.10.1
 */
PluginServer::PluginServer(void* handle, std::filesystem::path path)
    : handle_(handle), path_(std::move(path)) {}

/**
 * @brief Destroy the plugin instance, then close the library.
 *
 * Order matters: the instance's destructor lives inside the library, so the
 * library must outlive it. destroy() therefore happens-after every other
 * call on the handle, and dlclose happens-after destroy().
 *
 * @req REQ-MCP-006
 * @version 2.10.1
 */
PluginServer::~PluginServer() {
    if (instance_ != nullptr && destroy_fn_ != nullptr) {
        destroy_fn_(instance_);
        instance_ = nullptr;
    }
    if (handle_ != nullptr) {
        dlclose(handle_);
        handle_ = nullptr;
    }
}

/**
 * @brief Load a plugin .so and construct its server instance.
 *
 * dlopen, then dlsym of all nine entry points, then the API-version
 * check, then entropic_create_server(). Every failure is typed and
 * logged with its path — a configured plugin never fails silently.
 *
 * @param path Plugin shared object path.
 * @param[out] out Receives the loaded plugin on success; left untouched
 *                 on failure.
 * @return ENTROPIC_OK on success;
 *         ENTROPIC_ERROR_PLUGIN_LOAD_FAILED when dlopen fails, an entry
 *         point is missing, or the factory yields a null/unnamed
 *         instance; ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH when the
 *         plugin reports a different API version.
 * @req REQ-MCP-005
 * @req REQ-MCP-004
 * @version 2.10.1
 */
entropic_error_t PluginServer::load(const std::filesystem::path& path,
                                    std::unique_ptr<PluginServer>& out) {
    // RTLD_LOCAL keeps plugin symbols out of the global namespace so two
    // plugins exporting the same entry-point names cannot collide.
    dlerror();  // clear any stale error before the call
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* err = dlerror();
        logger->error("Plugin load failed: dlopen('{}'): {}", path.string(),
                      (err != nullptr) ? err : "unknown error");
        return ENTROPIC_ERROR_PLUGIN_LOAD_FAILED;
    }

    // Adopt the handle immediately so every failure path below closes it.
    std::unique_ptr<PluginServer> plugin(new PluginServer(handle, path));
    auto rc = plugin->resolve_and_init();
    if (rc != ENTROPIC_OK) {
        return rc;
    }

    logger->info("Loaded MCP plugin '{}' from {}", plugin->name_,
                 path.string());
    out = std::move(plugin);
    return ENTROPIC_OK;
}

/**
 * @brief Resolve entry points, check version, create the instance.
 * @return ENTROPIC_OK when the plugin satisfies the whole ABI contract;
 *         otherwise the first typed failure (load-failed or
 *         version-mismatch) from the step that failed.
 * @req REQ-MCP-004
 * @version 2.10.1
 */
entropic_error_t PluginServer::resolve_and_init() {
    auto rc = resolve_and_check_version();
    if (rc != ENTROPIC_OK) {
        return rc;
    }
    return create_instance();
}

/**
 * @brief Create the plugin's server instance and read its name.
 *
 * The name string is server-owned for the handle's lifetime (not
 * caller-freed), and it doubles as the tool-routing prefix — which is
 * why a blank name is a load failure rather than a warning.
 *
 * @return ENTROPIC_OK when the factory yields a non-null instance with a
 *         non-empty name; ENTROPIC_ERROR_PLUGIN_LOAD_FAILED otherwise.
 * @req REQ-MCP-004
 * @req REQ-MCP-005
 * @version 2.10.1
 */
entropic_error_t PluginServer::create_instance() {
    instance_ = create_fn_();
    if (instance_ == nullptr) {
        logger->error("Plugin load failed: entropic_create_server() returned "
                      "NULL for {}", path_.string());
        return ENTROPIC_ERROR_PLUGIN_LOAD_FAILED;
    }

    const char* raw_name = name_fn_(instance_);
    name_ = (raw_name != nullptr) ? raw_name : "";
    if (name_.empty()) {
        logger->error("Plugin load failed: entropic_mcp_server_name() gave an "
                      "empty name for {} — the name is the tool-routing "
                      "prefix and cannot be blank", path_.string());
        return ENTROPIC_ERROR_PLUGIN_LOAD_FAILED;
    }
    return ENTROPIC_OK;
}

/**
 * @brief Resolve every entry point and verify the reported API version.
 *
 * Signatures and the opaque handle type are LOCKED at API version 1, so
 * a plugin reporting anything other than
 * ENTROPIC_MCP_PLUGIN_API_VERSION is refused rather than called.
 *
 * @return ENTROPIC_OK when all nine symbols resolve and the version
 *         matches; ENTROPIC_ERROR_PLUGIN_LOAD_FAILED for a missing entry
 *         point; ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH for a version
 *         mismatch.
 * @req REQ-MCP-004
 * @req REQ-MCP-005
 * @version 2.10.1
 */
entropic_error_t PluginServer::resolve_and_check_version() {
    if (!resolve_symbols()) {
        return ENTROPIC_ERROR_PLUGIN_LOAD_FAILED;
    }

    const int reported = version_fn_();
    if (reported != ENTROPIC_MCP_PLUGIN_API_VERSION) {
        logger->error("Plugin version mismatch for {}: plugin reports API "
                      "version {}, this engine implements {}. Rebuild the "
                      "plugin against matching entropic headers.",
                      path_.string(), reported,
                      ENTROPIC_MCP_PLUGIN_API_VERSION);
        return ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH;
    }
    return ENTROPIC_OK;
}

/**
 * @brief dlsym all nine entry points, logging each one that is missing.
 *
 * Reports every absent symbol rather than stopping at the first, so a
 * partially-implemented plugin is diagnosed in one pass.
 *
 * The nine names are the whole plugin C ABI; each is declared
 * ENTROPIC_EXPORT in the interface header so a plugin built the usual
 * way (-fvisibility=hidden) still exports them and every dlsym here
 * resolves.
 *
 * @return true when all nine entry points resolved, false when any is
 *         absent (each missing name having been logged).
 * @req REQ-MCP-004
 * @version 2.10.1
 */
bool PluginServer::resolve_symbols() {
    // Both halves run unconditionally: a plugin missing several entry points
    // should be told about all of them in one build/run cycle.
    const bool factory_ok = resolve_factory_symbols();
    const bool instance_ok = resolve_instance_symbols();
    return factory_ok && instance_ok;
}

/**
 * @brief dlsym one entry point, logging and flagging when it is absent.
 * @param symbol_name Symbol to resolve.
 * @param[in,out] ok Cleared to false when the symbol is missing.
 * @return Symbol address, or nullptr.
 * @utility
 * @version 2.10.1
 */
void* PluginServer::resolve_one(const char* symbol_name, bool& ok) const {
    // The void*-to-function-pointer round trip is the POSIX dlsym idiom;
    // conditionally-supported in ISO C++ but well-defined on every platform
    // this engine targets.
    void* addr = dlsym(handle_, symbol_name);
    if (addr == nullptr) {
        logger->error("Plugin {}: missing required entry point '{}'",
                      path_.string(), symbol_name);
        ok = false;
    }
    return addr;
}

/**
 * @brief Resolve the process-level entry points (version, factory, name, free).
 * @return true when all four resolved, false when any is absent.
 * @req REQ-MCP-004
 * @version 2.10.1
 */
bool PluginServer::resolve_factory_symbols() {
    bool ok = true;
    version_fn_ = reinterpret_cast<version_fn_t>(
        resolve_one("entropic_plugin_api_version", ok));
    create_fn_ = reinterpret_cast<create_fn_t>(
        resolve_one("entropic_create_server", ok));
    name_fn_ = reinterpret_cast<name_fn_t>(
        resolve_one("entropic_mcp_server_name", ok));
    free_fn_ = reinterpret_cast<free_fn_t>(
        resolve_one("entropic_free", ok));
    return ok;
}

/**
 * @brief Resolve the per-instance entry points.
 * @return true when all five resolved, false when any is absent.
 * @req REQ-MCP-004
 * @version 2.10.1
 */
bool PluginServer::resolve_instance_symbols() {
    bool ok = true;
    list_tools_fn_ = reinterpret_cast<list_tools_fn_t>(
        resolve_one("entropic_mcp_server_list_tools", ok));
    execute_fn_ = reinterpret_cast<execute_fn_t>(
        resolve_one("entropic_mcp_server_execute", ok));
    configure_fn_ = reinterpret_cast<configure_fn_t>(
        resolve_one("entropic_mcp_server_configure", ok));
    set_dir_fn_ = reinterpret_cast<set_dir_fn_t>(
        resolve_one("entropic_mcp_server_set_working_dir", ok));
    destroy_fn_ = reinterpret_cast<destroy_fn_t>(
        resolve_one("entropic_mcp_server_destroy", ok));
    return ok;
}

/**
 * @brief Copy a plugin-allocated string and release it via the plugin.
 *
 * Strings from list_tools/execute are caller-owned and must be released
 * through THAT plugin's own entropic_free, never the engine's allocator
 * — the free is routed back through free_fn_ even on the throwing path.
 *
 * @param raw Plugin-allocated string; may be null.
 * @return Owned copy of the plugin's string, or an empty string when raw
 *         is null. The plugin's buffer is always released before return.
 * @req REQ-MCP-004
 * @version 2.10.1
 */
std::string PluginServer::take_string(char* raw) const {
    if (raw == nullptr) {
        return {};
    }
    std::string copy;
    try {
        copy.assign(raw);
    } catch (...) {
        free_fn_(raw);
        throw;
    }
    free_fn_(raw);
    return copy;
}

/**
 * @brief List the plugin's tools.
 *
 * Descriptors use `inputSchema` (camelCase), which is what
 * ServerManager::get_tool_schema parses so a plugin tool gets the same
 * argument validation a built-in does.
 *
 * @return JSON array string of tool descriptors; "[]" when the plugin
 *         returned nothing.
 * @req REQ-MCP-007
 * @req REQ-MCP-008
 * @req REQ-MCP-006
 * @version 2.10.1
 */
std::string PluginServer::list_tools() const {
    std::lock_guard<std::mutex> lock(call_mutex_);
    auto tools = take_string(list_tools_fn_(instance_));
    return tools.empty() ? "[]" : tools;
}

/**
 * @brief Execute one of the plugin's tools.
 *
 * Serialised on a per-instance mutex so the threading promise the plugin
 * header makes to authors — "the engine never runs these concurrently on
 * one handle" — holds by construction rather than by every upstream
 * caller behaving. No ordering is promised between distinct handles and
 * the calling thread is not fixed.
 *
 * @param tool_name Local tool name (no server prefix); borrowed for the
 *                  call.
 * @param args_json JSON arguments; borrowed for the call.
 * @return The plugin's ServerResponse JSON envelope, copied out of
 *         plugin-owned memory; empty when the plugin returned null.
 * @req REQ-MCP-006
 * @req REQ-MCP-002
 * @req REQ-MCP-007
 * @version 2.10.1
 */
std::string PluginServer::execute(const std::string& tool_name,
                                  const std::string& args_json) {
    std::lock_guard<std::mutex> lock(call_mutex_);
    return take_string(
        execute_fn_(instance_, tool_name.c_str(), args_json.c_str()));
}

/**
 * @brief Pass configuration to the plugin instance.
 *
 * Serialised on the same per-instance mutex as execute().
 *
 * @param config_json JSON configuration string; borrowed for the call.
 * @return The plugin's own entropic_error_t result, forwarded verbatim.
 * @req REQ-MCP-006
 * @version 2.10.1
 */
entropic_error_t PluginServer::configure(const std::string& config_json) {
    std::lock_guard<std::mutex> lock(call_mutex_);
    return configure_fn_(instance_, config_json.c_str());
}

/**
 * @brief Set the plugin's working directory.
 *
 * Serialised on the same per-instance mutex as execute().
 *
 * @param dir Directory path; borrowed for the call.
 * @return The plugin's own entropic_error_t result, forwarded verbatim.
 * @req REQ-MCP-006
 * @version 2.10.1
 */
entropic_error_t PluginServer::set_working_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(call_mutex_);
    return set_dir_fn_(instance_, dir.c_str());
}

} // namespace entropic
