// SPDX-License-Identifier: Apache-2.0
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
#include <entropic/core/compaction.h>
#include <entropic/core/engine.h>
#include <entropic/mcp/external_client.h>
#include <entropic/mcp/transport.h>
#include "mock_inference.h"
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
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

// ── gh#158: per-session-key run concurrency (v2.13.0) ───

/**
 * @brief Build an engine over a mock inference interface.
 * @param iface Mock interface.
 * @return Engine bound to it.
 * @utility
 * @version 2.13.0
 */
static entropic::AgentEngine make_session_engine(
    entropic::InferenceInterface& iface) {
    static entropic::LoopConfig lc;
    static entropic::CompactionConfig cc;
    return entropic::AgentEngine(iface, lc, cc);
}

SCENARIO("gh#158: the run guard is scoped to the session key",
         "[concurrency][gh158][2.13.0]") {
    GIVEN("an engine with per-session concurrency enabled") {
        entropic::test::MockInference mock;
        auto iface = entropic::test::make_mock_interface(mock);
        auto engine = make_session_engine(iface);
        engine.set_concurrent_sessions(true);

        WHEN("session A claims a turn") {
            REQUIRE(engine.try_begin_turn("A"));

            THEN("a second claim on A is refused") {
                CHECK_FALSE(engine.try_begin_turn("A"));
            }
            AND_THEN("a claim on B proceeds concurrently") {
                CHECK(engine.try_begin_turn("B"));
                CHECK(engine.active_run_count() == 2);
                engine.end_turn("B");
            }
            engine.end_turn("A");
            AND_THEN("releasing A leaves nothing running") {
                CHECK(engine.active_run_count() == 0);
                CHECK_FALSE(engine.is_running());
            }
        }
    }
}

SCENARIO("gh#158: the kill switch restores handle-wide serialization",
         "[concurrency][gh158][2.13.0]") {
    GIVEN("an engine with concurrent_sessions turned OFF") {
        // The default moved to ON once the decision-#66 audit completed,
        // so what needs a test is the escape hatch: `concurrent_sessions:
        // false` must reproduce v2.12.0 exactly, which is the whole reason
        // the key survives as a kill switch rather than being deleted.
        entropic::test::MockInference mock;
        auto iface = entropic::test::make_mock_interface(mock);
        auto engine = make_session_engine(iface);
        engine.set_concurrent_sessions(false);

        WHEN("session A claims a turn") {
            REQUIRE(engine.try_begin_turn("A"));

            THEN("a DIFFERENT key is refused too — v2.12.0 semantics") {
                CHECK_FALSE(engine.try_begin_turn("B"));
            }
            engine.end_turn("A");
        }
    }
}

SCENARIO("gh#158: concurrent sessions are ON by default",
         "[concurrency][gh158][2.13.0]") {
    GIVEN("an engine left at the shipped default") {
        entropic::test::MockInference mock;
        auto iface = entropic::test::make_mock_interface(mock);
        auto engine = make_session_engine(iface);

        WHEN("session A claims a turn") {
            REQUIRE(engine.try_begin_turn("A"));

            THEN("a different key proceeds without any opt-in") {
                CHECK(engine.concurrent_sessions());
                CHECK(engine.try_begin_turn("B"));
                CHECK(engine.active_run_count() == 2);
                engine.end_turn("B");
            }
            AND_THEN("the same key is still refused") {
                CHECK_FALSE(engine.try_begin_turn("A"));
            }
            engine.end_turn("A");
        }
    }
}

SCENARIO("gh#158: eight threads racing two keys yield exactly two winners",
         "[concurrency][gh158][2.13.0]") {
    GIVEN("an engine with per-session concurrency enabled") {
        entropic::test::MockInference mock;
        auto iface = entropic::test::make_mock_interface(mock);
        auto engine = make_session_engine(iface);
        engine.set_concurrent_sessions(true);

        std::atomic<int> winners{0};
        std::atomic<bool> go{false};

        WHEN("four threads claim 'A' and four claim 'B' simultaneously") {
            std::vector<std::thread> threads;
            for (int i = 0; i < 8; ++i) {
                threads.emplace_back([&, i] {
                    while (!go.load()) { /* tighten the race */ }
                    if (engine.try_begin_turn(i < 4 ? "A" : "B")) {
                        winners.fetch_add(1);
                    }
                });
            }
            go.store(true);
            for (auto& t : threads) { t.join(); }

            THEN("exactly one per key wins") {
                CHECK(winners.load() == 2);
                CHECK(engine.active_run_count() == 2);
            }
        }
    }
}

SCENARIO("gh#158: interrupt_session stops exactly one run",
         "[concurrency][gh158][2.13.0]") {
    GIVEN("two concurrent runs, A and B") {
        entropic::test::MockInference mock;
        auto iface = entropic::test::make_mock_interface(mock);
        auto engine = make_session_engine(iface);
        engine.set_concurrent_sessions(true);

        REQUIRE(engine.try_begin_turn("A"));
        REQUIRE(engine.try_begin_turn("B"));

        WHEN("A is interrupted by key") {
            engine.interrupt_session("A");

            THEN("only A's run carries the interrupt") {
                CHECK(engine.session_interrupted("A"));
                CHECK_FALSE(engine.session_interrupted("B"));
            }
        }

        WHEN("the handle-wide interrupt fires") {
            engine.interrupt();

            THEN("both runs carry it") {
                CHECK(engine.session_interrupted("A"));
                CHECK(engine.session_interrupted("B"));
            }
        }

        engine.end_turn("A");
        engine.end_turn("B");
    }
}

// ── gh#158 audit: the per-handle state a turn touches (v2.13.0) ──

/**
 * @brief Build a message list of the given size for token counting.
 * @param n Number of messages.
 * @return Messages with distinct content.
 * @utility
 * @version 2.13.0
 */
static std::vector<entropic::Message> make_messages(int n) {
    std::vector<entropic::Message> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        entropic::Message m;
        m.role = "user";
        m.content = "message number " + std::to_string(i)
                  + " with enough text to be worth counting";
        out.push_back(std::move(m));
    }
    return out;
}

SCENARIO("gh#158: token counting is safe from two concurrent runs",
         "[concurrency][gh158][2.13.0]") {
    GIVEN("one TokenCounter, the shape AgentEngine holds it in") {
        // The engine owns exactly ONE TokenCounter, shared by the
        // CompactionManager and by every context_usage() call. Before the
        // gh#158 audit, count_message() wrote a memo into an unsynchronized
        // unordered_map from a const method — so two runs counting at the
        // same time inserted into the same map concurrently. That is not a
        // wrong number; it is a corrupted heap.
        entropic::TokenCounter counter(8192);
        auto messages = make_messages(40);

        WHEN("8 threads count the same messages repeatedly") {
            std::atomic<int> mismatches{0};
            const int expected = counter.count_messages(messages);
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        if (counter.count_messages(messages) != expected) {
                            mismatches.fetch_add(1);
                        }
                        auto pct = counter.usage_percent(messages);
                        (void)pct;
                    }
                });
            }
            for (auto& th : threads) { th.join(); }

            THEN("every thread agrees, and nothing was corrupted") {
                CHECK(mismatches.load() == 0);
                CHECK(counter.count_messages(messages) == expected);
            }
        }

        WHEN("counters race against a cache invalidation") {
            std::atomic<int> mismatches{0};
            const int expected = counter.count_messages(messages);
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        if (counter.count_messages(messages) != expected) {
                            mismatches.fetch_add(1);
                        }
                    }
                });
                threads.emplace_back([&] {
                    for (int i = 0; i < kIterations; ++i) {
                        counter.clear_cache();
                    }
                });
            }
            for (auto& th : threads) { th.join(); }

            THEN("counting is unaffected by invalidation") {
                CHECK(mismatches.load() == 0);
            }
        }
    }
}

SCENARIO("gh#158: two concurrent runs accumulate metrics safely",
         "[concurrency][gh158][2.13.0]") {
    GIVEN("one engine driving two sessions on different tiers") {
        entropic::test::MockInference mock;
        auto iface = entropic::test::make_mock_interface(mock);
        auto engine = make_session_engine(iface);
        engine.set_concurrent_sessions(true);

        WHEN("two runs finish repeatedly while a third thread reads "
             "entropic_metrics_json's two sources") {
            // per_tier_metrics_ is an unordered_map written at the END of
            // every run and copied by entropic_metrics_json. A rehash under
            // the reader's copy is undefined behaviour, and last_metrics_ is
            // a multi-field struct assigned wholesale — an unlocked reader
            // gets a torn snapshot. Both are handle-wide; only the run guard
            // kept them apart before gh#158.
            constexpr int kRuns = 60;
            std::atomic<bool> done{false};
            std::atomic<int> reads{0};
            std::atomic<int> negative{0};

            std::thread reader([&] {
                while (!done.load()) {
                    auto per_tier = engine.per_tier_metrics();
                    auto last = engine.last_loop_metrics();
                    if (last.iterations < 0 || last.tool_calls < 0) {
                        negative.fetch_add(1);
                    }
                    // Structural corruption signal: the two runners only
                    // ever produce these two keys, so any other key, or a
                    // third entry, means the copy read a map that was
                    // rehashing under it.
                    if (per_tier.size() > 2) { negative.fetch_add(1); }
                    for (const auto& [tier, m] : per_tier) {
                        if ((tier != "alpha" && tier != "beta")
                                || m.iterations < 0) {
                            negative.fetch_add(1);
                        }
                    }
                    reads.fetch_add(1);
                }
            });

            std::vector<std::thread> runners;
            for (int t = 0; t < 2; ++t) {
                runners.emplace_back([&, t] {
                    const std::string tier =
                        t == 0 ? "alpha" : "beta";
                    for (int i = 0; i < kRuns; ++i) {
                        entropic::Message m;
                        m.role = "user";
                        m.content = "go";
                        engine.run({m}, tier);
                    }
                });
            }
            for (auto& th : runners) { th.join(); }
            done.store(true);
            reader.join();

            THEN("both tiers are present and no reader saw garbage") {
                auto per_tier = engine.per_tier_metrics();
                CHECK(per_tier.count("alpha") == 1);
                CHECK(per_tier.count("beta") == 1);
                CHECK(negative.load() == 0);
                CHECK(reads.load() > 0);
            }
        }
    }
}

// ── gh#158: external MCP tool execution (v2.13.0) ───────

/**
 * @brief Transport stub that records ids and can answer the wrong one.
 *
 * Stands in for a stdio pipe without spawning a child. `stale_reply`
 * reproduces the state a real pipe is left in when a request is ABANDONED
 * (timeout, or a per-session interrupt tripping `request_cancelled()`
 * mid-read): the server's answer arrives late and is the first line the
 * NEXT request reads.
 *
 * @version 2.13.0
 */
class ScriptedTransport : public entropic::Transport {
public:
    /**
     * @brief Open the stub transport.
     * @return Always true.
     * @utility
     * @version 2.13.0
     */
    bool open() override { return true; }

    /**
     * @brief Close the stub transport.
     * @utility
     * @version 2.13.0
     */
    void close() override {}

    /**
     * @brief Whether the stub is connected.
     * @return Always true.
     * @utility
     * @version 2.13.0
     */
    bool is_connected() const override { return true; }

    /**
     * @brief Answer a JSON-RPC request, recording the id it carried.
     * @param request_json The request line.
     * @param timeout_ms Ignored.
     * @return A JSON-RPC response line.
     * @utility
     * @version 2.13.0
     */
    std::string send_request(const std::string& request_json,
                             uint32_t /*timeout_ms*/) override {
        auto req = nlohmann::json::parse(request_json);
        const int id = req.value("id", -1);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ids_.push_back(id);
        }
        // A desynced pipe hands back the PREVIOUS request's whole reply —
        // its id AND its body — because that is the line still sitting in
        // the pipe from a request that was abandoned.
        const int answered = stale_reply ? id - 1 : id;
        nlohmann::json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = answered;
        if (req.value("method", "") == "tools/list") {
            resp["result"]["tools"] = nlohmann::json::array();
        } else {
            resp["result"]["content"] = nlohmann::json::array(
                {{{"type", "text"},
                  {"text", payload_for(answered)}}});
        }
        return resp.dump();
    }

    /**
     * @brief Every id this transport was asked to answer.
     * @return Copy of the recorded id list.
     * @utility
     * @version 2.13.0
     */
    std::vector<int> ids() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ids_;
    }

    /// @brief When true, answer request N with id N-1 (a desynced pipe).
    bool stale_reply = false;

    /**
     * @brief The body this transport returns for a given request id.
     * @param id Request id.
     * @return Deterministic payload text naming that id.
     * @utility
     * @version 2.13.0
     */
    static std::string payload_for(int id) {
        return "RESULT-FOR-REQUEST-" + std::to_string(id);
    }

private:
    mutable std::mutex mutex_;    ///< Guards ids_
    std::vector<int> ids_;        ///< Ids seen, in arrival order
};

SCENARIO("gh#158: a response that answers another request is refused",
         "[concurrency][gh158][mcp][2.13.0]") {
    GIVEN("an external client whose pipe is one reply behind") {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* raw = transport.get();
        raw->stale_reply = true;
        entropic::ExternalMCPClient client("srv", std::move(transport));

        WHEN("a tool is called") {
            auto envelope = client.execute("read_file", "{}");
            auto parsed = nlohmann::json::parse(envelope);

            THEN("the other request's payload is NOT returned as a result") {
                // This is the cross-session leak in its smallest form: the
                // body is real content that belongs to a DIFFERENT call, so
                // a caller has no way to tell it apart from its own.
                const auto text = parsed.value("result", std::string{});
                const auto ids = raw->ids();
                REQUIRE(ids.size() == 1);
                CHECK(text.find(
                    ScriptedTransport::payload_for(ids[0] - 1))
                        == std::string::npos);
            }
            AND_THEN("it is reported as an error, not silently dropped") {
                CHECK(parsed.value("is_error", false));
            }
        }
    }
}

SCENARIO("gh#158: concurrent tool calls on one client get distinct ids",
         "[concurrency][gh158][mcp][2.13.0]") {
    GIVEN("one external client, as ServerManager shares it per handle") {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* raw = transport.get();
        entropic::ExternalMCPClient client("srv", std::move(transport));

        WHEN("8 threads execute tools through it simultaneously") {
            // ServerManager holds ONE ExternalMCPClient per server and every
            // run routes through it, so two keyed runs call execute() on the
            // same object. next_id_ was a plain int: `next_id_++` from two
            // threads is a data race, and two requests sharing an id defeats
            // the pairing check that now guards the result.
            constexpr int kCalls = 200;
            std::atomic<int> wrong_payload{0};
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&] {
                    for (int i = 0; i < kCalls; ++i) {
                        auto envelope = client.execute("echo", "{}");
                        auto parsed = nlohmann::json::parse(envelope);
                        if (parsed.value("is_error", false)) {
                            wrong_payload.fetch_add(1);
                        }
                    }
                });
            }
            for (auto& th : threads) { th.join(); }

            THEN("every request carried a unique id") {
                auto ids = raw->ids();
                REQUIRE(ids.size()
                        == static_cast<size_t>(kThreads * kCalls));
                std::sort(ids.begin(), ids.end());
                auto dup = std::adjacent_find(ids.begin(), ids.end());
                CHECK(dup == ids.end());
            }
            AND_THEN("no call was answered by someone else's response") {
                CHECK(wrong_payload.load() == 0);
            }
        }
    }
}
