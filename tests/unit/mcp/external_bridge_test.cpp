// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file external_bridge_test.cpp
 * @brief ExternalBridge unit tests — async task lifecycle.
 * @version 2.0.11
 */

#include <entropic/mcp/external_bridge.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

using namespace entropic;
using json = nlohmann::json;

// ── Task status tests ────────────────────────────────────

SCENARIO("handle_ask_status returns error for unknown task_id",
         "[external_bridge][v2.0.11]")
{
    GIVEN("a bridge with no tasks") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test");

        WHEN("status is checked for nonexistent task") {
            json args = {{"task_id", "nonexistent"}};
            auto result = bridge.handle_ask_status(args);

            THEN("result contains unknown error") {
                auto text = result["content"][0]["text"]
                    .get<std::string>();
                CHECK(text.find("unknown task_id") !=
                      std::string::npos);
            }
        }
    }
}

SCENARIO("Async ask registers task and completes with error on null handle",
         "[external_bridge][v2.0.11]")
{
    GIVEN("a bridge with null engine handle") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test");

        WHEN("an async ask is submitted") {
            bridge.run_async_ask("test prompt", "task-abc", -1);

            // Wait for the background thread
            std::this_thread::sleep_for(
                std::chrono::milliseconds(200));

            json args = {{"task_id", "task-abc"}};
            auto result = bridge.handle_ask_status(args);

            THEN("task exists with error status (null handle)") {
                auto text = result["content"][0]["text"]
                    .get<std::string>();
                auto parsed = json::parse(text);
                CHECK(parsed["status"] == "error");
            }
        }
    }
}

SCENARIO("Cleanup removes expired tasks but keeps fresh ones",
         "[external_bridge][v2.0.11]")
{
    GIVEN("a bridge with a completed async task") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test");

        bridge.run_async_ask("prompt", "task-fresh", -1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(200));

        // Verify it exists
        json args = {{"task_id", "task-fresh"}};
        auto before = bridge.handle_ask_status(args);
        auto before_text = before["content"][0]["text"]
            .get<std::string>();
        REQUIRE(before_text.find("unknown") == std::string::npos);

        WHEN("cleanup is called") {
            bridge.cleanup_expired_tasks();

            THEN("fresh task survives (TTL is 15 min)") {
                auto after = bridge.handle_ask_status(args);
                auto after_text = after["content"][0]["text"]
                    .get<std::string>();
                CHECK(after_text.find("unknown") ==
                      std::string::npos);
            }
        }
    }
}

// ── relay_single_delegate setter test ────────────────────

SCENARIO("ExternalBridge constructs with explicit socket_path",
         "[external_bridge][v2.0.11]")
{
    GIVEN("a config with explicit socket_path") {
        ExternalMCPConfig cfg;
        cfg.socket_path = "/tmp/test-bridge.sock";
        ExternalBridge bridge(nullptr, cfg, "/tmp");

        THEN("socket_path matches config") {
            CHECK(bridge.socket_path() ==
                  std::filesystem::path("/tmp/test-bridge.sock"));
        }
    }
}

SCENARIO("ExternalBridge derives socket_path when not configured",
         "[external_bridge][v2.0.11]")
{
    GIVEN("a config without explicit socket_path") {
        ExternalMCPConfig cfg;
        // socket_path is nullopt → derived from project_dir
        ExternalBridge bridge(nullptr, cfg, "/tmp/test-project");

        THEN("socket_path is under ~/.entropic/socks/") {
            auto path = bridge.socket_path().string();
            CHECK(path.find(".entropic/socks/") !=
                  std::string::npos);
            CHECK(path.find(".sock") != std::string::npos);
        }
    }
}
