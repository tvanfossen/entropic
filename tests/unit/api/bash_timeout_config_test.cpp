// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file bash_timeout_config_test.cpp
 * @brief v2.13.0: `mcp.bash.timeout_seconds` reaches the bash server a
 *        run actually dispatches to, and a nonsense value fails the load.
 *
 * Until v2.13.0 the bash server's 30 s timeout was a constructor default
 * nobody passed and nothing enforced. Enforcing it without a knob would
 * turn every build or test suite longer than 30 s into a kill, so the
 * limit is configurable — and this proves it end to end: handle config ->
 * ServerManager::init_builtins -> BashServer -> a real dispatch through
 * the handle's ToolExecutor.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>
#include <entropic/mcp/servers/bash.h>
#include "engine_handle.h"  // white-box: tool_executor, server_manager

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

/**
 * @brief RAII handle configured from a JSON `mcp` object.
 * @internal
 * @version 2.13.0
 */
struct Handle {
    entropic_handle_t h = nullptr;              ///< Handle under test
    entropic_error_t rc = ENTROPIC_ERROR_INVALID_STATE; ///< configure()

    /**
     * @brief Create and configure with `mcp_json` as the mcp section.
     * @param mcp_json JSON object text for "mcp".
     * @internal
     * @version 2.13.0
     */
    explicit Handle(const std::string& mcp_json) {
        std::string json =
            R"({"log_level":"WARN","permissions":{"auto_approve":true},)"
            R"("mcp":)" + mcp_json + "}";
        entropic_create(&h);
        if (h != nullptr) { rc = entropic_configure(h, json.c_str()); }
    }
    /** @brief Destroy. @internal @version 2.13.0 */
    ~Handle() { entropic_destroy(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

/**
 * @brief The handle's default-set bash server.
 * @param h Configured handle.
 * @return The server, or nullptr.
 * @internal
 * @version 2.13.0
 */
entropic::BashServer* bash_of(entropic_handle_t h) {
    return dynamic_cast<entropic::BashServer*>(
        h->server_manager->get_server("bash"));
}

/**
 * @brief The `mcp` section rooted at /tmp with an optional bash object.
 * @param bash JSON for mcp.bash, or "" for none.
 * @return JSON object text.
 * @internal
 * @version 2.13.0
 */
std::string mcp_with(const std::string& bash) {
    std::string root = fs::temp_directory_path().string();
    std::string out = R"({"working_dir":")" + root + R"(")";
    if (!bash.empty()) { out += R"(,"bash":)" + bash; }
    return out + "}";
}

}  // namespace

SCENARIO("mcp.bash.timeout_seconds reaches the dispatched bash server",
         "[api][bash][timeout][v2.13.0]") {
    GIVEN("no mcp.bash section") {
        Handle h(mcp_with(""));
        REQUIRE(h.rc == ENTROPIC_OK);
        THEN("the bash server runs with the documented 30 s default") {
            auto* bash = bash_of(h.h);
            REQUIRE(bash != nullptr);
            CHECK(bash->timeout() == 30);
        }
    }
    GIVEN("timeout_seconds: 1") {
        Handle h(mcp_with(R"({"timeout_seconds":1})"));
        REQUIRE(h.rc == ENTROPIC_OK);
        WHEN("a run dispatches a command that would take 20 s") {
            entropic::ToolCall call;
            call.id = "t1";
            call.name = "bash.execute";
            call.arguments["command"] = "sleep 20";
            call.arguments_json = R"({"command":"sleep 20"})";
            entropic::LoopContext ctx;
            auto t0 = std::chrono::steady_clock::now();
            auto msgs = h.h->tool_executor->process_tool_calls(ctx, {call});
            auto elapsed = std::chrono::steady_clock::now() - t0;
            THEN("it is killed at the configured limit, and says so") {
                REQUIRE(msgs.size() == 1);
                INFO("result: " << msgs.front().content);
                CHECK(elapsed < std::chrono::seconds(10));
                auto j = nlohmann::json::parse(msgs.front().content,
                                               nullptr, false);
                REQUIRE(j.is_object());
                CHECK(j.value("error", "") == "timeout");
                CHECK(j.value("timeout_seconds", -1) == 1);
            }
        }
    }
    GIVEN("timeout_seconds: 0") {
        Handle h(mcp_with(R"({"timeout_seconds":0})"));
        THEN("the configuration is refused — no silent clamp, no unbounded") {
            CHECK(h.rc != ENTROPIC_OK);
        }
    }
    GIVEN("timeout_seconds: -5") {
        Handle h(mcp_with(R"({"timeout_seconds":-5})"));
        THEN("the configuration is refused") {
            CHECK(h.rc != ENTROPIC_OK);
        }
    }
}
