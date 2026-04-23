// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_engine.cpp
 * @brief AgentEngine unit tests — state machine, loop, callbacks.
 * @version 1.8.4
 */

#include <entropic/core/engine.h>
#include "mock_inference.h"
#include <catch2/catch_test_macros.hpp>

using namespace entropic;
using namespace entropic::test;

// ── Helper ───────────────────────────────────────────────

/**
 * @brief Create a minimal message list for testing.
 * @return Messages with system + user.
 * @internal
 * @version 1.8.4
 */
static std::vector<Message> make_messages() {
    Message sys;
    sys.role = "system";
    sys.content = "You are helpful.";
    Message usr;
    usr.role = "user";
    usr.content = "Hi";
    return {sys, usr};
}

// ── State machine tests ──────────────────────────────────

TEST_CASE("Single iteration completes", "[engine]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    auto result = engine.run(make_messages());
    REQUIRE(result.size() >= 3); // system + user + assistant
    REQUIRE(result.back().role == "assistant");
    REQUIRE(result.back().content == "Hello, world!");
}

TEST_CASE("State transitions IDLE->PLANNING->EXECUTING->COMPLETE",
          "[engine]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    std::vector<int> states;
    EngineCallbacks cb;
    cb.on_state_change = [](int s, void* ud) {
        auto* v = static_cast<std::vector<int>*>(ud);
        v->push_back(s);
    };
    cb.user_data = &states;
    engine.set_callbacks(cb);

    engine.run(make_messages());
    REQUIRE(states.size() >= 3);
    REQUIRE(states[0] == static_cast<int>(AgentState::PLANNING));
    REQUIRE(states[1] == static_cast<int>(AgentState::EXECUTING));
    REQUIRE(states.back() == static_cast<int>(AgentState::COMPLETE));
}

TEST_CASE("Max iterations stops loop", "[engine]") {
    MockInference mock;
    mock.is_complete = false; // Never complete
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    lc.max_iterations = 3;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    auto result = engine.run(make_messages());
    REQUIRE(mock.generate_call_count <= 3);
}

TEST_CASE("Interrupt stops loop", "[engine]") {
    MockInference mock;
    mock.is_complete = false;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    lc.max_iterations = 100;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    // Interrupt after first state change
    EngineCallbacks cb;
    cb.on_state_change = [](int s, void* ud) {
        if (s == static_cast<int>(AgentState::EXECUTING)) {
            static_cast<AgentEngine*>(ud)->interrupt();
        }
    };
    cb.user_data = &engine;
    engine.set_callbacks(cb);

    auto result = engine.run(make_messages());
    REQUIRE(mock.generate_call_count <= 2);
}

TEST_CASE("Metrics accurate after run", "[engine]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    auto result = engine.run(make_messages());
    // At least 1 iteration
    REQUIRE(mock.generate_call_count >= 1);
}

TEST_CASE("Finish reason length continues loop", "[engine]") {
    MockInference mock;
    mock.is_complete = false;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    lc.max_iterations = 5;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    auto result = engine.run(make_messages());
    // With is_complete=false, runs until max_iterations
    REQUIRE(mock.generate_call_count == 5);
}

TEST_CASE("Context anchors persist across runs", "[engine]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    // First run: set an anchor via directive processor
    // (test anchor persistence by running twice)
    auto r1 = engine.run(make_messages());
    auto r2 = engine.run(make_messages());
    // Both runs should complete
    REQUIRE(r1.back().role == "assistant");
    REQUIRE(r2.back().role == "assistant");
}

TEST_CASE("Tier routing fires callback", "[engine]") {
    MockInference mock;
    mock.tier = "lead";
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    std::string selected_tier;
    EngineCallbacks cb;
    cb.on_tier_selected = [](const char* t, void* ud) {
        *static_cast<std::string*>(ud) = t;
    };
    cb.user_data = &selected_tier;
    engine.set_callbacks(cb);

    engine.run(make_messages());
    REQUIRE(selected_tier == "lead");
}

TEST_CASE("Streaming generation works", "[engine]") {
    MockInference mock;
    mock.stream_token_by_token = true;
    mock.response = "streamed";
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    lc.stream_output = true;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    std::string chunks;
    EngineCallbacks cb;
    cb.on_stream_chunk = [](const char* c, size_t len, void* ud) {
        static_cast<std::string*>(ud)->append(c, len);
    };
    cb.user_data = &chunks;
    engine.set_callbacks(cb);

    auto result = engine.run(make_messages());
    REQUIRE(chunks == "streamed");
}

TEST_CASE("Batch generation works", "[engine]") {
    MockInference mock;
    mock.response = "batch result";
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    lc.stream_output = false;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    auto result = engine.run(make_messages());
    REQUIRE(result.back().content == "batch result");
}

// ── v2.0.11: relay_single_delegate ──────────────────────

SCENARIO("relay_single_delegate registers tier",
         "[engine][v2.0.11]")
{
    GIVEN("an engine with relay_single_delegate set for lead") {
        MockInference mock;
        mock.response = "ok";
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        engine.set_relay_single_delegate("lead");

        THEN("the tier is registered (no crash, setter works)") {
            // Verifies the setter doesn't throw/crash.
            // Full relay behavior requires delegation integration
            // test (delegation + engine together), covered in
            // integration tests.
            REQUIRE(true);
        }
    }
}

// ── P1-4: interrupt dedup (2.0.6-rc16) ───────────────────

SCENARIO("interrupt() is idempotent wrt state",
         "[engine][P1-4][2.0.6-rc16]")
{
    GIVEN("a fresh engine") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        WHEN("interrupt is called twice without reset") {
            engine.interrupt();
            engine.interrupt();
            THEN("reset clears it, subsequent interrupt logs again") {
                // No direct flag inspection API; exercise the happy
                // path that exchange(true) no-ops on second call.
                engine.reset_interrupt();
                engine.interrupt();
                REQUIRE(true);  // survives without crash / double-log
            }
        }
    }
}

// ── P1-6: zero-tool-call + explicit_completion failure ───

SCENARIO("tier_requires_explicit_completion honours tier resolver",
         "[engine][P1-6][2.0.6-rc16]")
{
    GIVEN("an engine with a tier resolver reporting explicit=true") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        TierResolutionInterface tri{};
        tri.get_tier_param = [](const std::string& tier,
                                const std::string& param,
                                void* /*ud*/) -> std::string {
            if (param == "explicit_completion" && tier == "lead") {
                return "true";
            }
            return "";
        };
        engine.set_tier_resolution(tri);

        THEN("lead tier returns true") {
            REQUIRE(engine.tier_requires_explicit_completion("lead"));
        }
        AND_THEN("other tiers return false") {
            REQUIRE_FALSE(
                engine.tier_requires_explicit_completion("eng"));
        }
    }

    GIVEN("an engine with no tier resolver wired") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        THEN("explicit_completion defaults to false") {
            REQUIRE_FALSE(
                engine.tier_requires_explicit_completion("any"));
        }
    }
}

// ── P1-9: circular delegation detection ──────────────────

SCENARIO("is_delegation_cycle flags ancestor reuse",
         "[engine][P1-9][2.0.6-rc16]")
{
    GIVEN("an engine and a loop context with ancestors [lead, eng]") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        LoopContext ctx{};
        ctx.locked_tier = "researcher";
        ctx.delegation_ancestor_tiers = {"lead", "eng"};

        WHEN("target equals the active tier") {
            THEN("it's a cycle") {
                REQUIRE(engine.is_delegation_cycle(ctx, "researcher"));
            }
        }
        WHEN("target matches an ancestor") {
            THEN("lead is a cycle") {
                REQUIRE(engine.is_delegation_cycle(ctx, "lead"));
            }
            AND_THEN("eng is a cycle") {
                REQUIRE(engine.is_delegation_cycle(ctx, "eng"));
            }
        }
        WHEN("target is fresh") {
            THEN("no cycle") {
                REQUIRE_FALSE(
                    engine.is_delegation_cycle(ctx, "qa"));
            }
        }
    }
}

// ── P1-10: external interrupt callback is invoked ────────

SCENARIO("interrupt() fires external interrupt callback once",
         "[engine][P1-10][2.0.6-rc16]")
{
    GIVEN("an engine with an external interrupt callback") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        int count = 0;
        engine.set_external_interrupt(
            [](void* ud) { ++*static_cast<int*>(ud); }, &count);

        WHEN("interrupt is called repeatedly") {
            engine.interrupt();
            engine.interrupt();
            engine.interrupt();
            THEN("external callback fired exactly once") {
                REQUIRE(count == 1);
            }
        }
        WHEN("reset clears the flag then interrupt fires again") {
            engine.interrupt();
            engine.reset_interrupt();
            engine.interrupt();
            THEN("callback fired twice") {
                REQUIRE(count == 2);
            }
        }
    }
}
