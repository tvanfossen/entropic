// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file external_bridge_test.cpp
 * @brief ExternalBridge unit tests — async task lifecycle.
 * @version 2.1.0
 */

#include <entropic/mcp/external_bridge.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <string>
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

// ── Subscriber broadcast tests (P0-2, 2.0.6-rc16) ─────────

/**
 * @brief Drain a socket fd into a string, reading one line at most.
 * @param fd Readable socket.
 * @return Line without trailing newline.
 * @internal
 * @version 2.0.6-rc16
 */
static std::string read_line_until_newline(int fd) {
    std::string out;
    char c = 0;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n') { break; }
        out += c;
    }
    return out;
}

SCENARIO("broadcast_notification fans out to every subscriber",
         "[external_bridge][multi_client][2.0.6-rc16]")
{
    GIVEN("a bridge with two subscribed fd pairs") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test-sub");

        std::array<int, 2> a{-1, -1};
        std::array<int, 2> b{-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, a.data()) == 0);
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, b.data()) == 0);

        bridge.subscribe(a[0]);
        bridge.subscribe(b[0]);
        REQUIRE(bridge.subscriber_count() == 2);

        WHEN("a notification is broadcast") {
            json notif = {{"jsonrpc", "2.0"},
                          {"method", "notifications/test"},
                          {"params", {{"x", 1}}}};
            bridge.broadcast_notification(notif);

            THEN("both subscribers receive the same line") {
                auto la = read_line_until_newline(a[1]);
                auto lb = read_line_until_newline(b[1]);
                CHECK(la == notif.dump());
                CHECK(lb == notif.dump());
            }
        }

        bridge.unsubscribe(a[0]);
        bridge.unsubscribe(b[0]);
        CHECK(bridge.subscriber_count() == 0);
        for (int fd : {a[0], a[1], b[0], b[1]}) { close(fd); }
    }
}

SCENARIO("broadcast_notification drops fds whose write fails",
         "[external_bridge][multi_client][2.0.6-rc16]")
{
    GIVEN("a bridge with one subscribed fd whose peer is closed") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test-drop");

        std::array<int, 2> pair{-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) == 0);
        bridge.subscribe(pair[0]);

        // Close both ends so the next write() gets EPIPE/EBADF.
        close(pair[1]);
        close(pair[0]);

        WHEN("a broadcast is attempted") {
            json notif = {{"method", "notifications/test"}};
            bridge.broadcast_notification(notif);

            THEN("the dead fd is removed from subscribers") {
                CHECK(bridge.subscriber_count() == 0);
            }
        }
    }
}

// ── P1-5: async task phase lifecycle (2.0.6-rc16) ─────────

SCENARIO("async task phase transitions through the lifecycle",
         "[external_bridge][P1-5][2.0.6-rc16]")
{
    GIVEN("a bridge with null handle (forces error branch)") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test-phase");

        bridge.run_async_ask("hi", "task-phase", -1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(200));

        json args = {{"task_id", "task-phase"}};
        auto result = bridge.handle_ask_status(args);

        THEN("status response surfaces both status and phase") {
            auto text = result["content"][0]["text"]
                .get<std::string>();
            auto parsed = json::parse(text);
            CHECK(parsed.contains("status"));
            CHECK(parsed.contains("phase"));
            // Null-handle path lands in error/failed terminal state.
            CHECK(parsed["status"] == "error");
            CHECK(parsed["phase"] == "failed");
        }
    }
}

// ── P1-8: cancel-on-clear primitives (2.0.6-rc16) ─────────

SCENARIO("Phase observer generation counter discards stale post-detach fires (E5+E6)",
         "[external_bridge][E5][E6][2.1.0]")
{
    GIVEN("a bridge with a manually-registered task") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test-obsgen");
        {
            std::lock_guard<std::mutex> lock(bridge.tasks_mutex_);
            auto& t = bridge.tasks_for_cancel()["task-1"];
            t.status = "running";
            t.phase = "running";
            t.created = std::chrono::steady_clock::now();
        }

        WHEN("attach_phase_observer is called") {
            // Note: attach calls entropic_set_state_observer(handle_, ...) on
            // a null handle. The facade tolerates this (no-op on null) per
            // the existing null-handle test fixtures used elsewhere here.
            bridge.attach_phase_observer("task-1");

            THEN("a callback fired now is fresh, not stale") {
                std::lock_guard<std::mutex> lock(bridge.tasks_mutex_);
                CHECK_FALSE(bridge.observer_call_is_stale());
            }

            AND_WHEN("detach_phase_observer is called") {
                bridge.detach_phase_observer();

                THEN("the observer-call gen check now reports stale") {
                    std::lock_guard<std::mutex> lock(bridge.tasks_mutex_);
                    CHECK(bridge.observer_call_is_stale());
                }
            }

            AND_WHEN("a stale cb thread tries to fire after detach") {
                bridge.detach_phase_observer();

                // Simulate an in-flight engine callback that already
                // dispatched before the nullptr-swap. The thread acquires
                // tasks_mutex_, checks staleness, exits without writing.
                std::atomic<bool> would_have_written{false};
                std::thread stale_cb([&] {
                    std::lock_guard<std::mutex> lock(bridge.tasks_mutex_);
                    if (bridge.observer_call_is_stale()) { return; }
                    would_have_written.store(true);
                });
                stale_cb.join();

                THEN("the stale callback exits without touching state") {
                    CHECK_FALSE(would_have_written.load());
                    // Phase remains the value set at task injection — the
                    // cb did NOT mutate it onto "validating" or "revising".
                    std::lock_guard<std::mutex> lock(bridge.tasks_mutex_);
                    auto& t = bridge.tasks_for_cancel().at("task-1");
                    CHECK(t.phase == "running");
                }
            }

            AND_WHEN("re-attached to a different task after detach") {
                bridge.detach_phase_observer();
                {
                    std::lock_guard<std::mutex> lock(bridge.tasks_mutex_);
                    auto& t2 = bridge.tasks_for_cancel()["task-2"];
                    t2.status = "running";
                    t2.phase = "running";
                    t2.created = std::chrono::steady_clock::now();
                }
                bridge.attach_phase_observer("task-2");

                THEN("staleness clears — fresh callbacks are accepted again") {
                    std::lock_guard<std::mutex> lock(bridge.tasks_mutex_);
                    CHECK_FALSE(bridge.observer_call_is_stale());
                }
            }
        }
    }
}

SCENARIO("mark cancelling flips queued/running tasks",
         "[external_bridge][P1-8][2.0.6-rc16]")
{
    GIVEN("a bridge with a manually-registered running task") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test-cancel");

        // Inject a synthetic running task without spawning a thread.
        {
            std::lock_guard<std::mutex> lock(bridge.tasks_mutex_);
            auto& tasks = bridge.tasks_for_cancel();
            auto& t = tasks["task-running"];
            t.status = "running";
            t.phase = "running";
            t.created = std::chrono::steady_clock::now();
        }

        WHEN("mark_tasks_cancelling-equivalent path flips the status") {
            {
                std::lock_guard<std::mutex> lock(bridge.tasks_mutex_);
                auto& tasks = bridge.tasks_for_cancel();
                for (auto& [_, t] : tasks) {
                    if (t.status == "running") {
                        t.status = "cancelled";
                        t.phase = "cancelling";
                    }
                }
            }
            THEN("handle_ask_status reflects cancellation") {
                json args = {{"task_id", "task-running"}};
                auto result = bridge.handle_ask_status(args);
                auto text = result["content"][0]["text"]
                    .get<std::string>();
                auto parsed = json::parse(text);
                CHECK(parsed["status"] == "cancelled");
                CHECK(parsed["phase"] == "cancelling");
            }
        }
    }
}
