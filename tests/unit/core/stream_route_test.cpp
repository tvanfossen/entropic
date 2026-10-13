// SPDX-License-Identifier: Apache-2.0
/**
 * @file stream_route_test.cpp
 * @brief gh#208 (v2.14.2): two concurrent streaming turns each stream only
 *        their own tokens, and neither outlives the other's stack.
 *
 * run_streaming built its stream context on its own stack and stored the
 * pointer in the ONE handle-wide callbacks_ slot. With concurrent sessions
 * (the default since v2.13.0, and routine under `entropic serve`) a second
 * streaming turn overwrote the slot: the first turn's tokens went to the
 * second turn's consumer, and once the second returned, the first streamed
 * into a dead frame (consumer-reported SIGSEGV, 2 runs in 3).
 *
 * Deterministic here: run A streams a long reply slowly; run B starts once
 * A is mid-stream, streams a short reply, and returns first. Each consumer
 * must receive exactly its own reply — the routing assertion fails on the
 * pre-fix code without depending on the crash.
 *
 * CPU-only against MockInference.
 *
 * @version 2.14.2
 */

#include <entropic/core/engine.h>
#include "mock_inference.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using namespace entropic;
using namespace entropic::test;

namespace {

/// @brief One consumer's collected stream.
struct Collector {
    std::mutex mu;
    std::string text;
    std::atomic<int> tokens{0};
    static void on_token(const char* t, size_t n, void* ud) {
        auto* c = static_cast<Collector*>(ud);
        std::lock_guard<std::mutex> lock(c->mu);
        c->text.append(t, n);
        ++c->tokens;
    }
    std::string get() {
        std::lock_guard<std::mutex> lock(mu);
        return text;
    }
};

}  // namespace

SCENARIO("gh#208: concurrent streaming turns each receive only their own tokens",
         "[engine][stream][gh208][concurrency][2.14.2]") {
    MockInference mock;
    mock.stream_token_by_token = true;
    mock.token_delay_ms = 3;
    const std::string long_reply(120, 'A');   // ~360 ms
    const std::string short_reply(10, 'B');   // ~30 ms
    mock.response_queue = {long_reply, short_reply};
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    Collector a;
    Collector b;
    int rc_a = -1;
    int rc_b = -1;
    std::thread ta([&] {
        engine.set_active_session("sess-a");
        rc_a = engine.run_streaming("long please", Collector::on_token, &a, nullptr);
    });
    // B starts only once A is streaming, so A's run claimed the slot first
    // and B's (pre-fix) overwrote it mid-stream.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (a.tokens.load() < 5 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::thread tb([&] {
        engine.set_active_session("sess-b");
        rc_b = engine.run_streaming("short please", Collector::on_token, &b, nullptr);
    });
    tb.join();
    ta.join();

    THEN("both runs complete") {
        CHECK(rc_a == 0);
        CHECK(rc_b == 0);
    }
    THEN("each consumer received exactly its own reply, and nothing of the other's") {
        INFO("A got [" << a.get() << "]");
        INFO("B got [" << b.get() << "]");
        CHECK(a.get() == long_reply);
        CHECK(b.get() == short_reply);
    }
}
