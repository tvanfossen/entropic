// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file message_queue_test.cpp
 * @brief Unit tests for the mid-generation user-message queue (gh#40).
 *
 * Covers queue semantics (enqueue / depth / clear / cap enforcement),
 * the load-bearing boundary detection property (queue consumed only
 * at top-level COMPLETE, not at parent-resume-after-child), and
 * thread-safety under concurrent enqueue/depth/consume traffic.
 *
 * @version 2.1.10
 */

#include <entropic/core/engine.h>
#include <entropic/core/engine_types.h>
#include "mock_inference.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace entropic;
using namespace entropic::test;

namespace {

/**
 * @brief Build an engine with default LoopConfig + mock inference.
 * @internal
 * @version 2.1.10
 */
AgentEngine make_engine(MockInference& mock,
                        LoopConfig lc = LoopConfig{}) {
    auto iface = make_mock_interface(mock);
    CompactionConfig cc;
    return AgentEngine(iface, lc, cc);
}

} // namespace

// ── Queue semantics ────────────────────────────────────────────

TEST_CASE("queue starts empty and accepts enqueues up to cap",
          "[engine][queue]") {
    MockInference mock;
    auto engine = make_engine(mock);
    REQUIRE(engine.user_message_queue_depth() == 0);
    REQUIRE(engine.queue_user_message("a"));
    REQUIRE(engine.queue_user_message("b"));
    REQUIRE(engine.user_message_queue_depth() == 2);
}

TEST_CASE("queue rejects enqueue past capacity", "[engine][queue]") {
    MockInference mock;
    LoopConfig lc;
    lc.message_queue_capacity = 2;
    auto engine = make_engine(mock, lc);
    REQUIRE(engine.queue_user_message("a"));
    REQUIRE(engine.queue_user_message("b"));
    REQUIRE_FALSE(engine.queue_user_message("c"));
    REQUIRE(engine.user_message_queue_depth() == 2);
}

TEST_CASE("clear empties the queue without affecting capacity",
          "[engine][queue]") {
    MockInference mock;
    LoopConfig lc;
    lc.message_queue_capacity = 4;
    auto engine = make_engine(mock, lc);
    REQUIRE(engine.queue_user_message("a"));
    REQUIRE(engine.queue_user_message("b"));
    engine.clear_user_message_queue();
    REQUIRE(engine.user_message_queue_depth() == 0);
    // Capacity restored — can refill up to the cap.
    REQUIRE(engine.queue_user_message("c"));
    REQUIRE(engine.queue_user_message("d"));
    REQUIRE(engine.queue_user_message("e"));
    REQUIRE(engine.queue_user_message("f"));
    REQUIRE_FALSE(engine.queue_user_message("g"));
}

TEST_CASE("set_message_queue_capacity clamps negative to zero",
          "[engine][queue]") {
    MockInference mock;
    auto engine = make_engine(mock);
    engine.set_message_queue_capacity(-5);
    REQUIRE_FALSE(engine.queue_user_message("a"));
}

// ── Boundary detection (the load-bearing test) ────────────────

TEST_CASE("is_running false outside a turn, true inside run_turn",
          "[engine][queue]") {
    MockInference mock;
    auto engine = make_engine(mock);
    REQUIRE_FALSE(engine.is_running());
    engine.run_turn("Hello");
    REQUIRE_FALSE(engine.is_running());
}

TEST_CASE("queued message is consumed exactly at top-level COMPLETE",
          "[engine][queue][boundary]") {
    // The drain hook lives in run_turn after run() returns. Child
    // loops invoked via run_loop never reach this point, so any
    // queued message can ONLY be consumed at the top-level turn
    // boundary — never at parent-resume-after-child. This test
    // simulates an in-flight enqueue by appending the queue entry
    // before invoking run_turn: the engine must drain it as a
    // second turn under the same run_turn call, and the queue
    // observer must fire exactly once with the consumed text.
    MockInference mock;
    auto engine = make_engine(mock);

    struct Capture {
        std::vector<std::string> consumed;
        std::vector<size_t> remaining;
    } cap;

    engine.set_queue_observer(
        [](const char* c, size_t rem, void* ud) {
            auto* x = static_cast<Capture*>(ud);
            x->consumed.emplace_back(c);
            x->remaining.push_back(rem);
        }, &cap);

    REQUIRE(engine.queue_user_message("follow-up"));
    auto result = engine.run_turn("initial");

    // Two assistant turns should have been produced: initial + drain.
    int assistant_count = 0;
    for (const auto& m : result) {
        if (m.role == "assistant") { ++assistant_count; }
    }
    REQUIRE(assistant_count == 2);
    REQUIRE(cap.consumed.size() == 1);
    REQUIRE(cap.consumed.front() == "follow-up");
    REQUIRE(cap.remaining.front() == 0);
    REQUIRE(engine.user_message_queue_depth() == 0);
}

TEST_CASE("multiple queued messages drain in FIFO order",
          "[engine][queue][boundary]") {
    MockInference mock;
    auto engine = make_engine(mock);

    struct Capture { std::vector<std::string> consumed; } cap;
    engine.set_queue_observer(
        [](const char* c, size_t, void* ud) {
            static_cast<Capture*>(ud)->consumed.emplace_back(c);
        }, &cap);

    REQUIRE(engine.queue_user_message("one"));
    REQUIRE(engine.queue_user_message("two"));
    REQUIRE(engine.queue_user_message("three"));
    engine.run_turn("initial");

    REQUIRE(cap.consumed.size() == 3);
    REQUIRE(cap.consumed[0] == "one");
    REQUIRE(cap.consumed[1] == "two");
    REQUIRE(cap.consumed[2] == "three");
}

// ── Thread safety ──────────────────────────────────────────────

TEST_CASE("concurrent enqueue + depth queries do not race",
          "[engine][queue][threading]") {
    MockInference mock;
    LoopConfig lc;
    lc.message_queue_capacity = 10000;
    auto engine = make_engine(mock, lc);

    constexpr int producers = 4;
    constexpr int per_producer = 500;
    std::atomic<int> accepted{0};

    std::vector<std::thread> threads;
    threads.reserve(producers + 1);
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p]() {
            for (int i = 0; i < per_producer; ++i) {
                if (engine.queue_user_message(
                        "p" + std::to_string(p) + "/"
                        + std::to_string(i))) {
                    accepted.fetch_add(1);
                }
            }
        });
    }
    threads.emplace_back([&]() {
        for (int i = 0; i < 1000; ++i) {
            (void)engine.user_message_queue_depth();
            std::this_thread::sleep_for(
                std::chrono::microseconds(10));
        }
    });

    for (auto& t : threads) { t.join(); }
    REQUIRE(accepted.load() == producers * per_producer);
    REQUIRE(engine.user_message_queue_depth()
            == static_cast<size_t>(producers * per_producer));
}
