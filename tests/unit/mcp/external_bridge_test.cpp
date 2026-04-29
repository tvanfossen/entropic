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
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
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

// ── Issue #4 (v2.1.2) regressions ────────────────────────
//
// Three independent bugs ganged together to deadlock the bridge against
// compliant MCP clients (entropic-explorer ↔ Claude Code reproducer):
//
//   A+B. ``notifications/ask_complete`` is non-spec, and shipped the
//        full result body inline. Some MCP clients silently buffer
//        unknown methods; combined with C, that buffering produced
//        hangs. We switched to ``notifications/progress`` (spec-defined)
//        with progressToken=task_id and dropped the result body —
//        consumers fetch via ``ask_status``.
//
//   C.   ``broadcast_notification`` did blocking ``write()``; a
//        non-draining peer wedged the async-task thread. Now uses
//        ``send(..., MSG_DONTWAIT)`` and drops the slow subscriber on
//        EAGAIN/EWOULDBLOCK.
//
//   D.   ``accept_loop`` called ``serve_client`` inline — only one
//        client at a time. Now spawns a per-client thread.
//
// Each test below pins one part of the contract so a future
// regression fails CI loudly.

namespace {

/**
 * @brief Capture the most recently broadcast notification by snooping
 *        a subscriber socket and parsing the JSON line.
 *
 * @param fd Socket fd that the bridge wrote to.
 * @return Parsed JSON, or empty object on read error.
 * @internal
 * @version 2.1.2
 */
json read_one_notification(int fd) {
    auto line = read_line_until_newline(fd);
    if (line.empty()) { return json::object(); }
    return json::parse(line, nullptr, /*allow_exceptions=*/false);
}

} // namespace

// ── (A+B): spec-compliant notifications/progress, no inline result ──

SCENARIO("Async ask emits notifications/progress (not ask_complete) "
         "with no inline result",
         "[external_bridge][regression][2.1.2][issue-4]")
{
    GIVEN("a bridge with one subscribed peer and a null engine handle") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test-issue4-ab");

        std::array<int, 2> sock{-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sock.data()) == 0);
        bridge.subscribe(sock[0]);

        WHEN("an async task completes (null handle → error branch)") {
            bridge.run_async_ask("prompt", "task-issue4-ab", -1);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            auto notif = read_one_notification(sock[1]);

            THEN("the method is the MCP-spec progress notification") {
                REQUIRE(notif.contains("method"));
                CHECK(notif["method"].get<std::string>() ==
                      "notifications/progress");
                // Pin the negation explicitly so a future revert to the
                // old method name fails this test, not just the positive
                // assertion above.
                CHECK(notif["method"].get<std::string>() !=
                      "notifications/ask_complete");
            }
            AND_THEN("progressToken correlates back to task_id") {
                REQUIRE(notif.contains("params"));
                CHECK(notif["params"]["progressToken"]
                      .get<std::string>() == "task-issue4-ab");
            }
            AND_THEN("the result body is NOT shipped inline") {
                // Pre-2.1.2 the notification carried a "result" or
                // "error" field with the full text. Post-fix, consumers
                // fetch via ask_status.
                CHECK_FALSE(notif["params"].contains("result"));
                CHECK_FALSE(notif["params"].contains("error"));
            }
            AND_THEN("status is preserved as the message field") {
                CHECK(notif["params"].contains("message"));
            }
        }

        bridge.unsubscribe(sock[0]);
        for (int fd : sock) { close(fd); }
    }
}

SCENARIO("Async ask notification method is in the MCP-spec allowed set",
         "[external_bridge][mcp_compliance][2.1.2][issue-4]")
{
    // Pin the spec-allowed notification methods (per modelcontextprotocol.io
    // 2025-06-18 spec). Any future change to run_async_ask's emitted
    // method name must remain in this set, or we're back in non-spec
    // territory and a future client could deadlock again.
    static const std::array<std::string, 9> kSpecMethods = {{
        "notifications/initialized",
        "notifications/cancelled",
        "notifications/progress",
        "notifications/message",
        "notifications/resources/updated",
        "notifications/resources/list_changed",
        "notifications/tools/list_changed",
        "notifications/prompts/list_changed",
        "notifications/roots/list_changed",
    }};

    GIVEN("a bridge with one subscribed peer") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test-issue4-spec");
        std::array<int, 2> sock{-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sock.data()) == 0);
        bridge.subscribe(sock[0]);

        WHEN("a completion notification fires") {
            bridge.run_async_ask("prompt", "task-spec", -1);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            auto notif = read_one_notification(sock[1]);

            THEN("the method name is in the MCP-spec allowed set") {
                REQUIRE(notif.contains("method"));
                auto m = notif["method"].get<std::string>();
                bool in_spec = false;
                for (const auto& allowed : kSpecMethods) {
                    if (allowed == m) { in_spec = true; break; }
                }
                CHECK(in_spec);
            }
        }

        bridge.unsubscribe(sock[0]);
        for (int fd : sock) { close(fd); }
    }
}

// ── (C): non-blocking broadcast, drop slow consumers ──

SCENARIO("broadcast_notification does not block when peer recv buffer is full",
         "[external_bridge][regression][2.1.2][issue-4]")
{
    GIVEN("a bridge with a subscribed peer that never reads") {
        ExternalMCPConfig cfg;
        ExternalBridge bridge(nullptr, cfg, "/tmp/test-issue4-c");

        std::array<int, 2> sock{-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sock.data()) == 0);

        // Shrink both buffers so we can fill them quickly. This is the
        // kernel-level setup that emulates a slow consumer: writes
        // beyond the buffer would block on a blocking socket. We are
        // proving the bridge's send-side does NOT block.
        int small = 4096;
        REQUIRE(setsockopt(sock[0], SOL_SOCKET, SO_SNDBUF,
                           &small, sizeof(small)) == 0);
        REQUIRE(setsockopt(sock[1], SOL_SOCKET, SO_RCVBUF,
                           &small, sizeof(small)) == 0);

        // Pre-fill the recv buffer of the peer so any further send
        // would EAGAIN.
        std::string filler(small * 2, 'x');
        ssize_t pre = ::send(sock[0], filler.c_str(), filler.size(),
                             MSG_DONTWAIT);
        // We don't strictly require the pre-fill to succeed — the test
        // is meaningful as long as the recv buffer ends up too full for
        // our notification to fit. Even a partial pre-fill works.
        (void)pre;

        bridge.subscribe(sock[0]);
        REQUIRE(bridge.subscriber_count() == 1);

        WHEN("a broadcast is attempted") {
            // Build a payload large enough that it definitely won't fit
            // alongside the pre-fill. 8KB notification, 4KB buffer.
            json notif = {
                {"jsonrpc", "2.0"},
                {"method", "notifications/progress"},
                {"params", {
                    {"progressToken", "slow-task"},
                    {"progress", 100},
                    {"total", 100},
                    {"message", std::string(8192, 'p')}
                }}
            };
            auto t0 = std::chrono::steady_clock::now();
            bridge.broadcast_notification(notif);
            auto elapsed = std::chrono::steady_clock::now() - t0;

            THEN("broadcast_notification returns within ~50ms (non-blocking)") {
                auto ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(elapsed).count();
                CHECK(ms < 50);
            }
            AND_THEN("the slow subscriber is dropped") {
                // Either the EAGAIN path fired and dropped, or the
                // partial-write path fired. Either way, subscriber_count
                // should now be 0 — the slow consumer is no longer in
                // the broadcast set.
                CHECK(bridge.subscriber_count() == 0);
            }
        }

        for (int fd : sock) { close(fd); }
    }
}

// ── (D): multi-client accept loop ──

SCENARIO("Bridge accepts multiple concurrent client connections",
         "[external_bridge][regression][2.1.2][issue-4]")
{
    // Pre-2.1.2 the accept loop called serve_client(fd) inline — only
    // ONE client could be connected at any time. A second connect
    // would queue in the kernel listen backlog and only be accepted
    // after the first disconnected. This test verifies that two
    // clients can both be connected simultaneously and both can issue
    // requests.
    GIVEN("a bridge bound to a private socket path") {
        ExternalMCPConfig cfg;
        // Per-test socket path so parallel test runs don't collide.
        // The third constructor arg is project_dir (used to derive a
        // socket path); set config.socket_path explicitly to override
        // that derivation and bind to a known location.
        std::string sock_path = "/tmp/test-issue4-d-" +
            std::to_string(::getpid()) + ".sock";
        ::unlink(sock_path.c_str());
        cfg.socket_path = sock_path;

        ExternalBridge bridge(nullptr, cfg, "/tmp");
        REQUIRE(bridge.start());

        // Helper: open a client connection to the bridge socket.
        auto connect_client = [&]() -> int {
            int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            REQUIRE(fd >= 0);
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, sock_path.c_str(),
                         sizeof(addr.sun_path) - 1);
            // Tight retry loop because start() is async; the listen()
            // call has happened but the accept thread may not have
            // poll()ed yet on the very first call.
            for (int i = 0; i < 50; ++i) {
                if (::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr)) == 0) { return fd; }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
            ::close(fd);
            return -1;
        };

        WHEN("two clients connect within a tight deadline") {
            // The (D) regression: pre-2.1.2, accept_loop called
            // serve_client inline, so the second client's connect()
            // would NOT complete until the first disconnected. We
            // verify both connects succeed concurrently — i.e. the
            // accept thread returned to poll() after each accept.
            //
            // We don't try to drive the dispatch path here; the bridge
            // is constructed with a null engine handle so JSON-RPC
            // request handling has nothing to dispatch against. The
            // connection-level acceptance IS the regression.
            int c1 = connect_client();
            int c2 = connect_client();

            THEN("both connections succeed concurrently") {
                CHECK(c1 >= 0);
                CHECK(c2 >= 0);
            }
            AND_THEN("each client write reaches the bridge without EPIPE") {
                // If the per-client thread for either fd hadn't been
                // spawned, send() to the dropped client would
                // eventually return EPIPE/ECONNRESET. We send a small
                // line on each and check the write doesn't error.
                std::string ping = R"({"jsonrpc":"2.0","method":"ping"})"
                                   "\n";
                ssize_t n1 = ::send(c1, ping.c_str(), ping.size(),
                                    MSG_NOSIGNAL);
                ssize_t n2 = ::send(c2, ping.c_str(), ping.size(),
                                    MSG_NOSIGNAL);
                CHECK(n1 == static_cast<ssize_t>(ping.size()));
                CHECK(n2 == static_cast<ssize_t>(ping.size()));
            }

            if (c1 >= 0) { ::close(c1); }
            if (c2 >= 0) { ::close(c2); }
        }

        bridge.stop();
        ::unlink(sock_path.c_str());
    }
}

SCENARIO("Bridge stop() cleanly joins per-client threads",
         "[external_bridge][regression][2.1.2][issue-4]")
{
    // Pre-2.1.2 stop() only joined the accept thread; per-client
    // serve_client ran inline so there were no other threads to join.
    // Post-fix, stop() must shutdown(fd) every connected client to
    // wake their blocking reads, then join their threads. A leaked
    // thread or a hang in stop() would manifest here.
    GIVEN("a started bridge with one connected client") {
        ExternalMCPConfig cfg;
        std::string sock_path = "/tmp/test-issue4-d-stop-" +
            std::to_string(::getpid()) + ".sock";
        ::unlink(sock_path.c_str());
        cfg.socket_path = sock_path;

        ExternalBridge bridge(nullptr, cfg, "/tmp");
        REQUIRE(bridge.start());

        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_path.c_str(),
                     sizeof(addr.sun_path) - 1);
        for (int i = 0; i < 50; ++i) {
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                          sizeof(addr)) == 0) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Give the per-client thread a moment to enter its read loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        WHEN("stop() is called while a client is still connected") {
            auto t0 = std::chrono::steady_clock::now();
            bridge.stop();
            auto elapsed = std::chrono::steady_clock::now() - t0;

            THEN("stop() returns within ~2s (no thread leak / hang)") {
                auto ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(elapsed).count();
                CHECK(ms < 2000);
            }
        }

        ::close(fd);
        ::unlink(sock_path.c_str());
    }
}
