// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_thread_safety.cpp
 * @brief Concurrency tests for thread-safe engine classes.
 *
 * Exercises ToolCallHistory, ProfileRegistry, ThroughputTracker,
 * MCPKeySet, and HookRegistry from multiple threads simultaneously.
 * Primary TSan targets — designed to provoke data races if
 * synchronization is incorrect.
 *
 * @version 1.9.14
 */

#include <entropic/mcp/tool_call_history.h>
#include <entropic/inference/profile_registry.h>
#include <entropic/inference/throughput_tracker.h>
#include <entropic/mcp/mcp_key_set.h>
#include <entropic/core/hook_registry.h>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace entropic;

// ── Constants ───────────────────────────────────────────

static constexpr int kThreads = 8;
static constexpr int kIterations = 500;

// ── ToolCallHistory ─────────────────────────────────────

SCENARIO("ToolCallHistory concurrent writes and reads",
         "[concurrency][tool_call_history]") {
    GIVEN("A shared ToolCallHistory") {
        ToolCallHistory history(64);
        std::atomic<int> seq{0};

        WHEN("8 threads write and read simultaneously") {
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        ToolCallRecord rec;
                        rec.sequence = seq.fetch_add(1);
                        rec.tool_name = "test.tool";
                        rec.status = "success";
                        rec.elapsed_ms = 1.0;
                        history.record(rec);

                        // Concurrent reads
                        auto r = history.recent(5);
                        auto s = history.size();
                        (void)r;
                        (void)s;
                    }
                });
            }
            for (auto& th : threads) th.join();

            THEN("no crash or corruption, size within capacity") {
                REQUIRE(history.size() <= 64);
                auto all = history.all();
                REQUIRE(all.size() == history.size());
            }
        }
    }
}

SCENARIO("ToolCallHistory concurrent to_json and record",
         "[concurrency][tool_call_history]") {
    GIVEN("A pre-populated ToolCallHistory") {
        ToolCallHistory history(32);
        for (int i = 0; i < 32; ++i) {
            history.record({static_cast<size_t>(i), "pre.fill",
                            "", "success", "", 1.0, "", 0});
        }

        WHEN("writers and JSON serializers run in parallel") {
            std::atomic<bool> saw_empty{false};
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        ToolCallRecord rec;
                        rec.tool_name = "conc.tool";
                        rec.status = "success";
                        history.record(rec);
                    }
                });
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        auto json = history.to_json(10);
                        if (json.empty()) saw_empty.store(true);
                    }
                });
            }
            for (auto& th : threads) th.join();

            THEN("to_json never returned empty") {
                REQUIRE_FALSE(saw_empty.load());
                REQUIRE(history.size() > 0);
            }
        }
    }
}

// ── ProfileRegistry ─────────────────────────────────────

SCENARIO("ProfileRegistry concurrent register and get",
         "[concurrency][profile_registry]") {
    GIVEN("A ProfileRegistry with bundled profiles loaded") {
        ProfileRegistry registry;
        registry.load_bundled();

        WHEN("threads register, deregister, and query in parallel") {
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t] {
                    for (int i = 0; i < kIterations; ++i) {
                        std::string name =
                            "custom_" + std::to_string(t);
                        GPUResourceProfile prof;
                        prof.name = name;
                        registry.register_profile(prof);
                        auto p = registry.get(name);
                        (void)p;
                        registry.has("balanced");
                        registry.deregister(name);
                    }
                });
            }
            for (auto& th : threads) th.join();

            THEN("bundled profiles still intact") {
                REQUIRE(registry.has("balanced"));
                REQUIRE(registry.has("maximum"));
            }
        }
    }
}

// ── ThroughputTracker ───────────────────────────────────

SCENARIO("ThroughputTracker concurrent record and read",
         "[concurrency][throughput_tracker]") {
    GIVEN("A shared ThroughputTracker") {
        ThroughputTracker tracker;

        WHEN("writers and readers run in parallel") {
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        tracker.record(100, 1000);
                        auto tps = tracker.tok_per_sec();
                        auto sc = tracker.sample_count();
                        auto pred = tracker.predict_ms(512);
                        auto rec = tracker.recommend_tokens(5000);
                        (void)tps;
                        (void)sc;
                        (void)pred;
                        (void)rec;
                    }
                });
            }
            for (auto& th : threads) th.join();

            THEN("samples recorded, no crash or NaN") {
                REQUIRE(tracker.sample_count() > 0);
                REQUIRE(tracker.tok_per_sec() > 0.0);
            }
        }
    }
}

SCENARIO("ThroughputTracker concurrent reset and record",
         "[concurrency][throughput_tracker]") {
    GIVEN("A ThroughputTracker with initial data") {
        ThroughputTracker tracker;
        tracker.record(200, 2000);

        WHEN("some threads record while others reset") {
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        tracker.record(50, 500);
                    }
                });
                threads.emplace_back([&] {
                    for (int i = 0; i < 50; ++i) {
                        tracker.reset();
                    }
                });
            }
            for (auto& th : threads) th.join();

            THEN("no crash — state is consistent") {
                // After all threads finish, tok_per_sec is either
                // 0 (just reset) or positive (recorded after reset).
                auto tps = tracker.tok_per_sec();
                REQUIRE(tps >= 0.0);
            }
        }
    }
}

// ── MCPKeySet ───────────────────────────────────────────

SCENARIO("MCPKeySet concurrent grant, revoke, and check",
         "[concurrency][mcp_key_set]") {
    GIVEN("A shared MCPKeySet") {
        MCPKeySet keys;

        WHEN("threads grant, revoke, and check access in parallel") {
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t] {
                    std::string pattern =
                        "server" + std::to_string(t) + ".*";
                    std::string tool =
                        "server" + std::to_string(t) + ".read";

                    for (int i = 0; i < kIterations; ++i) {
                        keys.grant(pattern, MCPAccessLevel::WRITE);
                        bool ok = keys.has_access(
                            tool, MCPAccessLevel::READ);
                        (void)ok;
                        keys.revoke(pattern);
                    }
                });
            }
            for (auto& th : threads) th.join();

            THEN("key set is empty after all revokes") {
                REQUIRE(keys.size() == 0);
            }
        }
    }
}

SCENARIO("MCPKeySet concurrent serialize and deserialize",
         "[concurrency][mcp_key_set]") {
    GIVEN("A MCPKeySet with initial grants") {
        MCPKeySet keys;
        keys.grant("filesystem.*", MCPAccessLevel::WRITE);
        keys.grant("git.*", MCPAccessLevel::READ);

        WHEN("readers serialize while writers modify") {
            std::atomic<bool> saw_empty{false};
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        auto json = keys.serialize();
                        if (json.empty()) saw_empty.store(true);
                    }
                });
                threads.emplace_back([&, t] {
                    std::string p = "extra" + std::to_string(t);
                    for (int i = 0; i < kIterations; ++i) {
                        keys.grant(p, MCPAccessLevel::READ);
                        keys.revoke(p);
                    }
                });
            }
            for (auto& th : threads) th.join();

            THEN("original grants survive, serialize never empty") {
                REQUIRE_FALSE(saw_empty.load());
                REQUIRE(keys.has_access("filesystem.read_file",
                                        MCPAccessLevel::WRITE));
            }
        }
    }
}

// ── HookRegistry ────────────────────────────────────────

/**
 * @brief No-op hook callback for concurrency testing.
 * @param hook_point Hook point (unused).
 * @param context_json Context (unused).
 * @param out_json Output pointer (set to NULL).
 * @param user_data User data (unused).
 * @return 0 (proceed).
 * @callback
 * @version 1.9.14
 */
static int noop_hook(
    entropic_hook_point_t /*hook_point*/,
    const char* /*context_json*/,
    char** out_json,
    void* /*user_data*/) {
    if (out_json) *out_json = nullptr;
    return 0;
}

SCENARIO("HookRegistry concurrent register and fire",
         "[concurrency][hook_registry]") {
    GIVEN("A shared HookRegistry") {
        HookRegistry registry;

        WHEN("threads register hooks while others fire them") {
            std::vector<std::thread> threads;

            // Registerers
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&, t] {
                    void* ud = reinterpret_cast<void*>(
                        static_cast<uintptr_t>(t));
                    for (int i = 0; i < kIterations; ++i) {
                        registry.register_hook(
                            ENTROPIC_HOOK_PRE_TOOL_CALL,
                            noop_hook, ud, t);
                        registry.deregister_hook(
                            ENTROPIC_HOOK_PRE_TOOL_CALL,
                            noop_hook, ud);
                    }
                });
            }

            // Firers
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        char* out = nullptr;
                        registry.fire_pre(
                            ENTROPIC_HOOK_PRE_TOOL_CALL,
                            "{}", &out);
                        if (out) {
                            delete[] out;
                        }
                    }
                });
            }

            for (auto& th : threads) th.join();

            THEN("no crash, all hooks deregistered") {
                REQUIRE(registry.hook_count(
                    ENTROPIC_HOOK_PRE_TOOL_CALL) == 0);
            }
        }
    }
}

SCENARIO("HookRegistry fire_info from multiple threads",
         "[concurrency][hook_registry]") {
    GIVEN("A registry with a registered info hook") {
        HookRegistry registry;
        std::atomic<int> fire_count{0};

        auto counter_hook = [](entropic_hook_point_t,
                               const char*,
                               char** out,
                               void* ud) -> int {
            auto* cnt = static_cast<std::atomic<int>*>(ud);
            cnt->fetch_add(1);
            if (out) *out = nullptr;
            return 0;
        };

        registry.register_hook(
            ENTROPIC_HOOK_ON_STATE_CHANGE,
            counter_hook, &fire_count, 0);

        WHEN("8 threads fire info hooks simultaneously") {
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        registry.fire_info(
                            ENTROPIC_HOOK_ON_STATE_CHANGE,
                            "{}");
                    }
                });
            }
            for (auto& th : threads) th.join();

            THEN("all fires counted, no lost or duplicated") {
                REQUIRE(fire_count.load() ==
                        kThreads * kIterations);
            }
        }
    }
}
