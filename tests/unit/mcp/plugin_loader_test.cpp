// SPDX-License-Identifier: Apache-2.0
/**
 * @file plugin_loader_test.cpp
 * @brief gh#133: ServerManager loads MCP server plugins via dlopen.
 *
 * `i_mcp_server.h` has documented a dlopen-based plugin contract since
 * v1.8.5 ("ServerManager discovers plugins via dlopen"), but no loader
 * existed — a consumer implementing the contract produced a `.so` that
 * nothing could load. These tests pin the loader that closes that gap.
 *
 * @par Test structure
 * The fixture plugin (`fixtures/test_plugin.cpp`) is built as a real
 * loadable module using ONLY the public interface header, so it stands in
 * for a genuine third-party plugin.
 *
 * 1. **Control** — dlopen/dlsym the fixture directly. This proves the
 *    fixture itself is conformant and, because it is compiled with
 *    `-fvisibility=hidden`, that the ENTROPIC_EXPORT declarations added in
 *    gh#133 actually export the entry points. If this fails, a failure in
 *    the tests below is the fixture's fault, not the engine's.
 * 2. **Loader** — the RED case: ServerManager must surface the plugin's
 *    tool in list_tools() and route execute() to it.
 * 3. **Failure paths** — a missing .so and a version-mismatched .so must
 *    both fail LOUD with their typed error, never silently continue with
 *    the plugin absent (an explicitly-configured plugin that vanishes
 *    without an error masks the user's intent).
 *
 * @version 2.10.1
 */

#include <entropic/interfaces/i_mcp_server.h>
#include <entropic/mcp/server_manager.h>
#include <entropic/types/config.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <dlfcn.h>

#include <filesystem>
#include <string>

using entropic::MCPConfig;
using entropic::PermissionsConfig;
using entropic::ServerManager;

namespace {

/**
 * @brief Path to the conformant fixture plugin built alongside these tests.
 * @return Absolute path to libentropic-test-plugin.so.
 * @version 2.10.1
 */
std::filesystem::path good_plugin() {
    return std::filesystem::path(TEST_PLUGIN_DIR) /
           "libentropic-test-plugin.so";
}

/**
 * @brief Path to the fixture plugin built with an incompatible API version.
 * @return Absolute path to libentropic-test-plugin-badversion.so.
 * @version 2.10.1
 */
std::filesystem::path bad_version_plugin() {
    return std::filesystem::path(TEST_PLUGIN_DIR) /
           "libentropic-test-plugin-badversion.so";
}

/**
 * @brief Build a ServerManager with no built-in servers registered.
 *
 * Keeps the assertions below about plugin tools only — no builtin tool
 * can accidentally satisfy them.
 *
 * @return Manager rooted at the current directory.
 * @version 2.10.1
 */
ServerManager make_manager() {
    return ServerManager(PermissionsConfig{}, std::filesystem::current_path());
}

/**
 * @brief Config naming a single plugin path.
 * @param path Plugin .so to load.
 * @return MCPConfig with every builtin disabled and one plugin listed.
 * @version 2.10.1
 */
MCPConfig config_with_plugin(const std::filesystem::path& path) {
    MCPConfig cfg;
    cfg.enable_entropic = false;
    cfg.enable_filesystem = false;
    cfg.enable_bash = false;
    cfg.enable_git = false;
    cfg.enable_diagnostics = false;
    cfg.enable_web = false;
    cfg.plugins.push_back(path);
    return cfg;
}

}  // namespace

TEST_CASE("gh#133 control: the fixture plugin exports the full contract",
          "[mcp][plugin][gh133][control]") {
    // Turns RED if ENTROPIC_EXPORT is missing from the i_mcp_server.h
    // declarations: the fixture builds with -fvisibility=hidden, so without
    // it GCC drops the visibility attribute and every dlsym below returns
    // null. Guarantees the loader tests fail for engine reasons only.
    REQUIRE(std::filesystem::is_regular_file(good_plugin()));

    void* handle = dlopen(good_plugin().c_str(), RTLD_NOW | RTLD_LOCAL);
    INFO("dlopen: " << (dlerror() != nullptr ? dlerror() : "ok"));
    REQUIRE(handle != nullptr);

    auto version_fn =
        reinterpret_cast<int (*)()>(dlsym(handle, "entropic_plugin_api_version"));
    auto create_fn = reinterpret_cast<entropic_mcp_server_t (*)()>(
        dlsym(handle, "entropic_create_server"));
    auto name_fn = reinterpret_cast<const char* (*)(entropic_mcp_server_t)>(
        dlsym(handle, "entropic_mcp_server_name"));
    auto list_fn = reinterpret_cast<char* (*)(entropic_mcp_server_t)>(
        dlsym(handle, "entropic_mcp_server_list_tools"));
    auto exec_fn =
        reinterpret_cast<char* (*)(entropic_mcp_server_t, const char*,
                                   const char*)>(
            dlsym(handle, "entropic_mcp_server_execute"));
    auto destroy_fn = reinterpret_cast<void (*)(entropic_mcp_server_t)>(
        dlsym(handle, "entropic_mcp_server_destroy"));
    auto free_fn =
        reinterpret_cast<void (*)(void*)>(dlsym(handle, "entropic_free"));

    REQUIRE(version_fn != nullptr);
    REQUIRE(create_fn != nullptr);
    REQUIRE(name_fn != nullptr);
    REQUIRE(list_fn != nullptr);
    REQUIRE(exec_fn != nullptr);
    REQUIRE(destroy_fn != nullptr);
    REQUIRE(free_fn != nullptr);

    CHECK(version_fn() == ENTROPIC_MCP_PLUGIN_API_VERSION);

    auto* server = create_fn();
    REQUIRE(server != nullptr);
    CHECK(std::string(name_fn(server)) == "testplugin");

    char* tools = list_fn(server);
    REQUIRE(tools != nullptr);
    auto parsed = nlohmann::json::parse(tools);
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0]["name"] == "echo");
    free_fn(tools);

    char* result = exec_fn(server, "echo", R"({"text":"hello"})");
    REQUIRE(result != nullptr);
    CHECK(nlohmann::json::parse(result)["result"] == "echo: hello");
    free_fn(result);

    destroy_fn(server);
    dlclose(handle);
}

TEST_CASE("gh#133 a configured plugin's tools appear in list_tools",
          "[mcp][plugin][gh133]") {
    auto manager = make_manager();
    auto cfg = config_with_plugin(good_plugin());

    REQUIRE(manager.load_plugins(cfg) == ENTROPIC_OK);

    auto tools = nlohmann::json::parse(manager.list_tools());
    bool found = false;
    for (const auto& tool : tools) {
        if (tool["name"] == "testplugin.echo") {
            found = true;
        }
    }
    INFO("tools: " << tools.dump());
    // The decisive gate: before gh#133 no loader existed, so a configured
    // plugin contributed nothing here.
    CHECK(found);
}

TEST_CASE("gh#133 a tool call routes through to the plugin",
          "[mcp][plugin][gh133]") {
    auto manager = make_manager();
    auto cfg = config_with_plugin(good_plugin());
    REQUIRE(manager.load_plugins(cfg) == ENTROPIC_OK);

    auto response =
        nlohmann::json::parse(manager.execute("testplugin.echo",
                                              R"({"text":"routed"})"));
    CHECK(response["result"] == "echo: routed");
}

TEST_CASE("gh#133 a plugin registers as a named server",
          "[mcp][plugin][gh133]") {
    auto manager = make_manager();
    auto cfg = config_with_plugin(good_plugin());
    REQUIRE(manager.load_plugins(cfg) == ENTROPIC_OK);

    auto names = manager.server_names();
    CHECK(std::find(names.begin(), names.end(), "testplugin") != names.end());

    auto info = manager.list_server_info();
    REQUIRE(info.count("testplugin") == 1);
    CHECK(info["testplugin"].transport == "plugin");
    CHECK(info["testplugin"].status == "connected");
}

TEST_CASE("gh#133 a missing plugin .so fails loud, not silently",
          "[mcp][plugin][gh133][failloud]") {
    // An explicitly-configured plugin that silently fails to load leaves the
    // engine running with the user's stated intent unmet — the typed error
    // is what gets it CORRECTED.
    auto manager = make_manager();
    auto cfg = config_with_plugin("/nonexistent/definitely-not-here.so");

    CHECK(manager.load_plugins(cfg) == ENTROPIC_ERROR_PLUGIN_LOAD_FAILED);
}

TEST_CASE("gh#133 a version-mismatched plugin is rejected with its own code",
          "[mcp][plugin][gh133][failloud]") {
    auto manager = make_manager();
    auto cfg = config_with_plugin(bad_version_plugin());

    REQUIRE(std::filesystem::is_regular_file(bad_version_plugin()));
    // Distinct from LOAD_FAILED: the .so opened fine, its contract is stale.
    CHECK(manager.load_plugins(cfg) ==
          ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH);

    // ...and it must NOT have been registered despite the rejection.
    auto names = manager.server_names();
    CHECK(std::find(names.begin(), names.end(), "badversion") == names.end());
}

TEST_CASE("gh#133 an empty plugin list is a no-op success",
          "[mcp][plugin][gh133]") {
    auto manager = make_manager();
    MCPConfig cfg;
    CHECK(manager.load_plugins(cfg) == ENTROPIC_OK);
}
