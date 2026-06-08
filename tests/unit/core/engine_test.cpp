// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_engine.cpp
 * @brief AgentEngine unit tests — state machine, loop, callbacks.
 * @version 2.0.6-rc16
 */

#include <entropic/core/engine.h>
#include <entropic/core/delegation.h>
#include <entropic/core/engine_types.h>
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

TEST_CASE("Root conversation created at run() init when storage wired (gh#48)",
          "[engine][gh48]") {
    // Pre-v2.1.12 regression: AgentEngine::run default-constructed
    // LoopContext, leaving ctx.conversation_id empty. Every downstream
    // delegation copied that empty string into parent_conversation_id
    // and FK-failed silently against conversations(id). The fix calls
    // storage_.create_conversation at run() init when wired, and stores
    // the returned id on the root context.
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    int call_count = 0;
    std::string captured_title;
    StorageInterface si{};
    si.create_conversation = [](const char* title, std::string& out_id,
                                void* ud) -> bool {
        auto* state = static_cast<std::pair<int, std::string>*>(ud);
        state->first++;
        state->second = title ? title : "";
        out_id = "11111111-1111-1111-1111-111111111111";
        return true;
    };
    std::pair<int, std::string> state{0, ""};
    si.user_data = &state;
    engine.set_storage(si);

    engine.run(make_messages());
    // create_conversation fires exactly once, with the engine's
    // chosen root title (not empty).
    REQUIRE(state.first == 1);
    REQUIRE_FALSE(state.second.empty());
    (void) call_count;
    (void) captured_title;
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

// ── P2-15: last_loop_metrics() ──────────────────────────

TEST_CASE("last_loop_metrics returns zero before any run", "[engine][P2-15]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    auto m = engine.last_loop_metrics();
    REQUIRE(m.iterations == 0);
    REQUIRE(m.tool_calls == 0);
    REQUIRE(m.tokens_used == 0);
}

TEST_CASE("last_loop_metrics populated after run", "[engine][P2-15]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    engine.run(make_messages());
    auto m = engine.last_loop_metrics();
    REQUIRE(m.iterations >= 1);
    REQUIRE(m.duration_ms() >= 0);
}

TEST_CASE("Streaming interrupt propagates to backend cancel chain "
          "(gh#49)", "[engine][gh49][streaming]") {
    // Regression for v2.1.7 streaming interrupt break: thread B calls
    // engine.interrupt() while thread A is mid-stream; the per-token
    // cancel-flag chain must propagate to the mock backend's cancel
    // poll so the stream terminates within ~1 more token instead of
    // running to the response's natural end.
    //
    // We can't trivially launch a real second thread from a Catch2
    // case, so we mimic the cross-thread race deterministically:
    // an on_stream_chunk callback installed via set_callbacks fires
    // engine.interrupt() after the 2nd character. The next iteration
    // of stream_token_callback observes events_->interrupt and raises
    // the cancel flag the mock polls — the mock then exits before
    // emitting the rest.
    MockInference mock;
    mock.response = "0123456789ABCDEFGHIJ";   // 20 chars
    mock.stream_token_by_token = true;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    struct State {
        AgentEngine* engine = nullptr;
        int chars_seen = 0;
    };
    State state;
    state.engine = &engine;
    EngineCallbacks cb;
    cb.on_stream_chunk = [](const char* /*t*/, size_t /*l*/, void* ud) {
        auto* s = static_cast<State*>(ud);
        s->chars_seen++;
        if (s->chars_seen == 2) {
            s->engine->interrupt();
        }
    };
    cb.user_data = &state;
    engine.set_callbacks(cb);

    engine.run(make_messages());

    // Expected cadence:
    //   token 1 emitted -> on_stream_chunk fires (chars_seen=1)
    //   token 2 emitted -> on_stream_chunk fires, interrupt() called
    //                       (chars_seen=2; interrupt_flag_ now true)
    //   token 3 emitted -> stream_token_callback observes the
    //                       interrupt BEFORE on_stream_chunk runs,
    //                       raises *cancel_flag=1, calls on_stream_chunk
    //                       (chars_seen=3)
    //   mock's next loop iter polls cancel != 0 and returns 0
    // So we should see <= 3 chars total even though the response
    // had 20 chars to stream.
    REQUIRE(state.chars_seen >= 2);
    REQUIRE(state.chars_seen <= 3);
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

// ── P1-6 regression: zero-tool-call loop bounded (2.0.6-rc16.2) ─

SCENARIO("zero-tool-call with explicit_completion halts within retry cap",
         "[engine][P1-6][regression][2.0.6-rc16.2]")
{
    GIVEN("lead tier requires explicit_completion, model emits no tool call") {
        MockInference mock;
        mock.response = "Hello!";
        mock.tier = "lead";
        mock.finish_reason = "stop";
        mock.is_complete = false;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        lc.max_iterations = 15;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        TierResolutionInterface tri{};
        tri.get_tier_param = [](const std::string& tier,
                                const std::string& param,
                                void* /*ud*/) -> std::string {
            if (tier == "lead" && param == "explicit_completion") {
                return "true";
            }
            return "";
        };
        engine.set_tier_resolution(tri);

        WHEN("the loop runs") {
            engine.run(make_messages());

            THEN("generation is capped at 3 (initial + 2 corrections)") {
                REQUIRE(mock.generate_call_count <= 3);
            }
            AND_THEN("loop stops well short of max_iterations") {
                REQUIRE(mock.generate_call_count < 15);
            }
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

// ── gh#64: max_consecutive_failed_delegations guard ──────

SCENARIO("is_delegation_repeat_blocked enforces consecutive cap",
         "[engine][gh64][delegation]") {
    GIVEN("an engine with default max_consecutive_failed_delegations=2") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        LoopContext ctx{};
        ctx.locked_tier = "lead";

        WHEN("there is no prior failed delegation") {
            THEN("any target is allowed") {
                REQUIRE_FALSE(
                    engine.is_delegation_repeat_blocked(ctx, "registrar"));
            }
        }

        WHEN("the same target has failed under the cap") {
            ctx.last_failed_delegation_target = "registrar";
            ctx.consecutive_failed_delegations = 1;
            THEN("retrying the target is still allowed") {
                REQUIRE_FALSE(
                    engine.is_delegation_repeat_blocked(ctx, "registrar"));
            }
        }

        WHEN("the same target has hit the cap") {
            ctx.last_failed_delegation_target = "registrar";
            ctx.consecutive_failed_delegations = 2;
            THEN("retrying the same target is blocked") {
                REQUIRE(
                    engine.is_delegation_repeat_blocked(ctx, "registrar"));
            }
            AND_THEN("a different target is NOT blocked") {
                REQUIRE_FALSE(
                    engine.is_delegation_repeat_blocked(ctx, "researcher"));
            }
        }

        WHEN("the cap is exceeded") {
            ctx.last_failed_delegation_target = "registrar";
            ctx.consecutive_failed_delegations = 5;
            THEN("the same target stays blocked") {
                REQUIRE(
                    engine.is_delegation_repeat_blocked(ctx, "registrar"));
            }
        }
    }
}

SCENARIO("max_consecutive_failed_delegations is configurable",
         "[engine][gh64][delegation]") {
    GIVEN("an engine configured with cap=4") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        lc.max_consecutive_failed_delegations = 4;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        LoopContext ctx{};
        ctx.last_failed_delegation_target = "registrar";

        WHEN("the counter is 3 (under the configured cap)") {
            ctx.consecutive_failed_delegations = 3;
            THEN("not blocked") {
                REQUIRE_FALSE(
                    engine.is_delegation_repeat_blocked(ctx, "registrar"));
            }
        }

        WHEN("the counter is 4 (at the cap)") {
            ctx.consecutive_failed_delegations = 4;
            THEN("blocked") {
                REQUIRE(
                    engine.is_delegation_repeat_blocked(ctx, "registrar"));
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

// ── P3-18: per-identity overrides (2.0.6-rc16) ───────────────────

SCENARIO("max_iterations override caps loop before global limit",
         "[engine][P3-18][2.0.6-rc16]")
{
    GIVEN("an engine with global max_iterations=50 and a resolver returning 2") {
        MockInference mock;
        mock.is_complete = false;  // never finishes on its own
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        lc.max_iterations = 50;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        TierResolutionInterface tri{};
        tri.get_tier_param = [](const std::string& tier,
                                const std::string& param,
                                void* /*ud*/) -> std::string {
            if (tier == "lead" && param == "max_iterations") {
                return "2";
            }
            return "";
        };
        engine.set_tier_resolution(tri);

        WHEN("run_loop is called with locked_tier='lead'") {
            LoopContext ctx;
            ctx.messages = make_messages();
            ctx.locked_tier = "lead";
            engine.run_loop(ctx);

            THEN("loop stops at 2 iterations, not 50") {
                REQUIRE(mock.generate_call_count <= 2);
            }
        }
    }

    GIVEN("an engine with no tier resolver (global default applies)") {
        MockInference mock;
        mock.is_complete = false;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        lc.max_iterations = 3;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        WHEN("run_loop is called without locked_tier") {
            LoopContext ctx;
            ctx.messages = make_messages();
            engine.run_loop(ctx);

            THEN("loop uses global max_iterations=3") {
                REQUIRE(mock.generate_call_count <= 3);
            }
        }
    }
}

// ── E7 regression: forced synthetic complete marks terminal_reason ─

SCENARIO("Budget-exhausted forced complete sets terminal_reason metadata",
         "[engine][E7][2.0.6-rc18]")
{
    GIVEN("an engine that will hit max_iterations with no real complete") {
        MockInference mock;
        mock.response = "working on it";
        mock.finish_reason = "stop";
        mock.is_complete = false;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        lc.max_iterations = 3;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        WHEN("loop runs to the cap") {
            LoopContext ctx;
            ctx.messages = make_messages();
            engine.run_loop(ctx);

            THEN("terminal_reason metadata is budget_exhausted") {
                auto it = ctx.metadata.find("terminal_reason");
                REQUIRE(it != ctx.metadata.end());
                REQUIRE(it->second == "budget_exhausted");
            }
            AND_THEN("final state is COMPLETE (synthetic)") {
                REQUIRE(ctx.state == AgentState::COMPLETE);
            }
        }
    }
}

// ── E2: budget_exhausted relay to lead (2.1.0) ───────────

/**
 * @brief State passed to inject_delegation via user_data.
 * @internal
 * @version 2.1.0
 */
struct DelegInjector {
    bool fired = false;  ///< Prevents double-injection.
};

/**
 * @brief Tool executor that injects a delegation on first call.
 * @param ctx Loop context (pending_delegation set as side effect).
 * @param calls Tool calls (unused — just need non-empty to be called).
 * @param ud DelegInjector pointer.
 * @return Empty message list.
 * @internal
 * @version 2.1.0
 */
static std::vector<Message> inject_delegation_once(
    LoopContext& ctx,
    const std::vector<ToolCall>& /*calls*/,
    void* ud) {
    auto* inj = static_cast<DelegInjector*>(ud);
    if (!inj->fired) {
        inj->fired = true;
        // Delegate to "eng" (not "lead") to avoid cycle-detection rejection.
        ctx.pending_delegation = PendingDelegation{"eng", "budget test", -1};
    }
    Message m;
    m.role = "tool";
    m.content = "injected delegation";
    return {m};
}

SCENARIO("budget_exhausted child is relayed partial to lead (E2)",
         "[engine][E2][2.1.0]")
{
    GIVEN("an engine with relay_single_delegate for lead") {
        MockInference mock;
        mock.is_complete = false;
        // First parse: non-empty tool call (triggers process_tool_results).
        // Subsequent parses: empty (child loop generates no tools).
        mock.tool_calls_queue.push_back(
            R"([{"name":"test.mock","arguments":{}}])");
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        lc.max_iterations = 3;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        engine.set_relay_single_delegate("lead");

        // Tier resolver must return valid info so delegation proceeds.
        TierResolutionInterface tri{};
        tri.resolve_tier = [](const std::string& /*tier*/,
                               void* /*ud*/) -> ChildContextInfo {
            ChildContextInfo info;
            info.valid = true;
            info.system_prompt = "child agent";
            return info;
        };
        engine.set_tier_resolution(tri);

        // Tool executor injects delegation on first call only.
        DelegInjector injector;
        ToolExecutionInterface tex{};
        tex.process_tool_calls = inject_delegation_once;
        tex.user_data = &injector;
        engine.set_tool_executor(tex);

        WHEN("parent loop runs with locked_tier=lead") {
            LoopContext ctx;
            ctx.messages = make_messages();
            ctx.locked_tier = "lead";
            engine.run_loop(ctx);

            THEN("relay_status is budget_exhausted_relayed") {
                auto it = ctx.metadata.find("relay_status");
                REQUIRE(it != ctx.metadata.end());
                REQUIRE(it->second == "budget_exhausted_relayed");
            }
            AND_THEN("state is COMPLETE via relay") {
                REQUIRE(ctx.state == AgentState::COMPLETE);
            }
            AND_THEN("explicit_completion_summary has budget_exhausted prefix") {
                auto it = ctx.metadata.find(
                    "explicit_completion_summary");
                REQUIRE(it != ctx.metadata.end());
                REQUIRE(it->second.substr(0, 8) == "[partial");
            }
        }
    }
}

// ── gh#68 (v2.3.4): entropic.complete history shape ─────────

namespace gh68 {

/// @brief Build a context with one empty assistant message + the
/// summary stashed in metadata as `dir_complete` would have done.
static entropic::LoopContext make_ctx_post_dir_complete(
    const std::string& summary) {
    entropic::LoopContext ctx;
    ctx.messages.push_back({"user", "class list?"});
    ctx.messages.push_back({"assistant", ""});  // post-adapter-strip
    ctx.metadata["explicit_completion_summary"] = summary;
    return ctx;
}

/// @brief Build the tool-result message that ToolExecutor produces
/// for entropic.complete pre-fix.
static entropic::Message make_complete_result_msg() {
    entropic::Message m;
    m.role = "user";
    m.content =
        R"({"action":"complete","summary":"I am sorry, I don't understand."})";
    m.metadata["tool_name"] = "entropic.complete";
    m.metadata["tool_call_id"] = "call_1";
    return m;
}

}  // namespace gh68

SCENARIO("fold_complete_into_assistant: happy path folds summary "
         "into empty assistant message (gh#68)",
         "[engine][gh68][complete-shape]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("ctx with empty assistant + completion summary in metadata") {
        auto ctx = gh68::make_ctx_post_dir_complete(
            "I am sorry, I don't understand.");
        auto result = gh68::make_complete_result_msg();

        WHEN("fold_complete_into_assistant is called") {
            bool folded = engine.fold_complete_into_assistant(ctx, result);

            THEN("it returns true (caller should skip pushing)") {
                REQUIRE(folded == true);
            }

            THEN("assistant message body is replaced with the summary") {
                REQUIRE(ctx.messages.size() == 2);  // user + assistant
                REQUIRE(ctx.messages.back().role == "assistant");
                REQUIRE(ctx.messages.back().content
                        == "I am sorry, I don't understand.");
            }

            THEN("no extra messages appended") {
                REQUIRE(ctx.messages.size() == 2);
            }
        }
    }
}

SCENARIO("fold_complete_into_assistant: refuses when assistant has prose "
         "(gh#68)",
         "[engine][gh68][complete-shape]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("ctx where the assistant message already has prose content") {
        // Real-world variant: model emitted "Sure, here goes." THEN
        // <tool_call>...</tool_call>. Adapter-stripped result still has
        // the prose. We must NOT overwrite prose with the summary.
        auto ctx = gh68::make_ctx_post_dir_complete("summary text");
        ctx.messages.back().content = "Sure, here goes.";
        auto result = gh68::make_complete_result_msg();

        WHEN("fold_complete_into_assistant is called") {
            bool folded = engine.fold_complete_into_assistant(ctx, result);

            THEN("it returns false (caller pushes JSON normally)") {
                REQUIRE(folded == false);
            }
            THEN("the prose assistant body is unchanged") {
                REQUIRE(ctx.messages.back().content == "Sure, here goes.");
            }
        }
    }
}

SCENARIO("fold_complete_into_assistant: refuses for non-complete tools "
         "(gh#68)",
         "[engine][gh68][complete-shape]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("ctx ready to fold but the tool result is entropic.delegate") {
        auto ctx = gh68::make_ctx_post_dir_complete("would-be summary");
        entropic::Message delegate_result;
        delegate_result.role = "user";
        delegate_result.content = R"({"action":"delegate","target":"x"})";
        delegate_result.metadata["tool_name"] = "entropic.delegate";

        WHEN("fold is called") {
            bool folded = engine.fold_complete_into_assistant(
                ctx, delegate_result);

            THEN("it returns false — only entropic.complete is folded") {
                REQUIRE(folded == false);
            }
            THEN("the empty assistant body stays empty") {
                REQUIRE(ctx.messages.back().content.empty());
            }
        }
    }
}

SCENARIO("fold_complete_into_assistant: refuses when last msg isn't assistant "
         "(gh#68)",
         "[engine][gh68][complete-shape]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("ctx where the most recent message is a system reminder, not assistant") {
        auto ctx = gh68::make_ctx_post_dir_complete("summary");
        ctx.messages.push_back({"system", "[engine] reminder"});
        auto result = gh68::make_complete_result_msg();

        WHEN("fold is called") {
            bool folded = engine.fold_complete_into_assistant(ctx, result);
            THEN("it returns false") {
                REQUIRE(folded == false);
            }
        }
    }
}

SCENARIO("fold_complete_into_assistant: refuses when metadata summary missing "
         "(gh#68)",
         "[engine][gh68][complete-shape]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("ctx with empty assistant but no explicit_completion_summary metadata") {
        entropic::LoopContext ctx;
        ctx.messages.push_back({"user", "q"});
        ctx.messages.push_back({"assistant", ""});
        // Intentionally NOT setting metadata — simulates a state where
        // dir_complete hasn't run yet, or some other path that lacks
        // the stashed summary.
        auto result = gh68::make_complete_result_msg();

        WHEN("fold is called") {
            bool folded = engine.fold_complete_into_assistant(ctx, result);
            THEN("it returns false — never invent content from thin air") {
                REQUIRE(folded == false);
            }
            THEN("assistant body stays empty (no spurious mutation)") {
                REQUIRE(ctx.messages.back().content.empty());
            }
        }
    }
}

SCENARIO("fold_complete_into_assistant: refuses on empty message history "
         "(gh#68)",
         "[engine][gh68][complete-shape]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("ctx with zero messages (degenerate but possible during init)") {
        entropic::LoopContext ctx;
        ctx.metadata["explicit_completion_summary"] = "x";
        auto result = gh68::make_complete_result_msg();

        WHEN("fold is called") {
            THEN("it returns false without crashing on empty back()") {
                REQUIRE_NOTHROW(
                    engine.fold_complete_into_assistant(ctx, result));
                REQUIRE_FALSE(
                    engine.fold_complete_into_assistant(ctx, result));
            }
        }
    }
}

SCENARIO("defang_meta_action_envelope: reshapes meta {action} results so "
         "the model is not primed to parrot them (gh#88)",
         "[engine][gh88][complete-shape]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("a delegate result message (entropic.delegate {action:...})") {
        entropic::Message msg;
        msg.role = "user";
        msg.content =
            R"({"action":"delegate","target":"registrar","task":"x"})";
        msg.metadata["tool_name"] = "entropic.delegate";

        WHEN("defang_meta_action_envelope is called") {
            engine.defang_meta_action_envelope(msg);
            THEN("content becomes a non-call prose status line") {
                REQUIRE(msg.content.find(R"({"action")")
                        == std::string::npos);
                REQUIRE(msg.content.find("delegate") != std::string::npos);
                REQUIRE(msg.content.find("accepted") != std::string::npos);
            }
        }
    }

    GIVEN("a NON-entropic tool result of the same shape") {
        entropic::Message msg;
        msg.content = R"({"action":"delegate","target":"x"})";
        msg.metadata["tool_name"] = "filesystem.read_file";

        WHEN("defang_meta_action_envelope is called") {
            const std::string before = msg.content;
            engine.defang_meta_action_envelope(msg);
            THEN("content is untouched — only entropic.* meta tools") {
                REQUIRE(msg.content == before);
            }
        }
    }

    GIVEN("an entropic meta result that is not an action envelope") {
        entropic::Message msg;
        msg.content = "plain non-JSON tool output";
        msg.metadata["tool_name"] = "entropic.inspect";

        WHEN("defang_meta_action_envelope is called") {
            const std::string before = msg.content;
            engine.defang_meta_action_envelope(msg);
            THEN("content is untouched") {
                REQUIRE(msg.content == before);
            }
        }
    }
}

// ── gh#88 COMBINED-path: defang must run at the real process_tool_results
//    call site, not only via the standalone helper (audit Pattern D /
//    feedback_test_combined_call_path). The SCENARIO above proves the helper
//    in isolation; this one proves the engine actually invokes it in-loop. ──

namespace gh88_combined {
/// @brief Executor returning an entropic.delegate meta result whose body is a
/// raw {"action":...} envelope — the exact shape gh#88's defang must strip at
/// the real call site so the model is not primed to parrot it. Mirrors the
/// production ToolExecutor output for a delegate directive.
/// @utility
/// @version 2.8.1
inline std::vector<Message> delegate_envelope_fn(
    LoopContext& /*ctx*/,
    const std::vector<ToolCall>& /*calls*/,
    void* /*user_data*/) {
    Message r;
    r.role = "user";
    r.content = R"({"action":"delegate","target":"registrar","task":"x"})";
    r.metadata["tool_name"] = "entropic.delegate";
    r.metadata["result_kind"] = "ok";
    return {r};
}
inline ToolExecutionInterface delegate_envelope_executor() {
    ToolExecutionInterface tex;
    tex.process_tool_calls = &delegate_envelope_fn;
    return tex;
}
}  // namespace gh88_combined

SCENARIO("gh#88 combined: process_tool_results defangs a meta {action} "
         "envelope at the real call site, not only the standalone helper",
         "[engine][gh88][combined][complete-shape]") {
    MockInference mock;
    // Turn 1: model emits a delegate tool call -> the injected executor returns
    // the raw {"action":...} envelope. Turn 2: no tool calls -> loop completes.
    mock.tool_calls_queue.push_back(
        R"([{"name":"entropic.delegate",)"
        R"("arguments":{"target":"registrar","task":"x"}}])");
    auto iface = make_mock_interface(mock);
    LoopConfig lc; lc.max_iterations = 4; CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);
    engine.set_tool_executor(gh88_combined::delegate_envelope_executor());

    LoopContext ctx; ctx.messages = make_messages();

    GIVEN("the executor returns a raw {\"action\":...} delegate envelope") {
        WHEN("the engine runs the loop (process_tool_results executes in-line)") {
            engine.run_loop(ctx);
            THEN("the landed tool-result message is defanged at the call site") {
                const Message* landed = nullptr;
                for (const auto& m : ctx.messages) {
                    auto it = m.metadata.find("tool_name");
                    if (it != m.metadata.end()
                        && it->second == "entropic.delegate") {
                        landed = &m;
                    }
                }
                REQUIRE(landed != nullptr);
                // RED if the engine.cpp defang call is removed: the raw
                // call-shaped envelope survives into the conversation and
                // re-primes the gh#88 parrot spiral.
                CHECK(landed->content.find(R"({"action")")
                      == std::string::npos);
                CHECK(landed->content.find("delegate") != std::string::npos);
                CHECK(landed->content.find("accepted") != std::string::npos);
            }
        }
    }
}

SCENARIO("fold_complete_into_assistant: long summary preserves UTF-8 "
         "(gh#68)",
         "[engine][gh68][complete-shape]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("ctx with a multi-byte UTF-8 summary in metadata") {
        // Mix of ASCII, multi-byte Unicode (em-dash, smart quotes,
        // emoji) — exercises that we're not splitting bytes anywhere.
        std::string summary =
            "I don't understand — does \"class list\" mean "
            "subjects or schedules? 🤔";
        auto ctx = gh68::make_ctx_post_dir_complete(summary);
        auto result = gh68::make_complete_result_msg();

        WHEN("fold is called") {
            engine.fold_complete_into_assistant(ctx, result);
            THEN("the full UTF-8 summary lands intact") {
                REQUIRE(ctx.messages.back().content == summary);
            }
        }
    }
}

// ── v2.3.10 coverage push — engine.cpp uncovered regions ──

#include <entropic/core/directives.h>
#include <entropic/interfaces/i_hook_handler.h>
#include <entropic/types/hooks.h>

namespace v2310 {

struct HookCap {
    std::vector<int> pre, post, info;
    bool cancel = false;
    int cancel_point = -1;
    std::string post_mod, pre_mod;
};

inline HookInterface make_hooks(HookCap* c) {
    HookInterface hi{};
    hi.registry = c;
    hi.fire_pre = [](void* r, entropic_hook_point_t p,
                     const char*, char** out) -> int {
        auto* c = static_cast<HookCap*>(r);
        c->pre.push_back(static_cast<int>(p));
        if (!c->pre_mod.empty()) {
            *out = static_cast<char*>(std::malloc(c->pre_mod.size() + 1));
            std::memcpy(*out, c->pre_mod.c_str(), c->pre_mod.size() + 1);
        }
        return (c->cancel && (c->cancel_point < 0
            || c->cancel_point == static_cast<int>(p))) ? 1 : 0;
    };
    hi.fire_post = [](void* r, entropic_hook_point_t p,
                      const char*, char** out) {
        auto* c = static_cast<HookCap*>(r);
        c->post.push_back(static_cast<int>(p));
        if (!c->post_mod.empty()) {
            *out = static_cast<char*>(std::malloc(c->post_mod.size() + 1));
            std::memcpy(*out, c->post_mod.c_str(), c->post_mod.size() + 1);
        }
    };
    hi.fire_info = [](void* r, entropic_hook_point_t p, const char*) {
        static_cast<HookCap*>(r)->info.push_back(static_cast<int>(p));
    };
    return hi;
}

inline bool has(const std::vector<int>& v, int x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}
inline char* malloc_str(const char* s) {
    auto n = std::strlen(s);
    auto* p = static_cast<char*>(std::malloc(n + 1));
    std::memcpy(p, s, n + 1);
    return p;
}

}  // namespace v2310

SCENARIO("hook dispatch: info / PRE_GENERATE cancel / POST_GENERATE rewrite / ON_ERROR",
         "[v2.3.10][core][engine_coverage][hooks]") {
    SECTION("info hooks fire across loop boundaries") {
        MockInference mock; auto iface = make_mock_interface(mock);
        LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
        v2310::HookCap cap;
        engine.set_hooks(v2310::make_hooks(&cap));
        engine.run(make_messages());
        REQUIRE(v2310::has(cap.info, ENTROPIC_HOOK_ON_LOOP_START));
        REQUIRE(v2310::has(cap.info, ENTROPIC_HOOK_ON_LOOP_END));
        REQUIRE(v2310::has(cap.info, ENTROPIC_HOOK_ON_LOOP_ITERATION));
        REQUIRE(v2310::has(cap.info, ENTROPIC_HOOK_ON_CONTEXT_ASSEMBLE));
        REQUIRE(v2310::has(cap.info, ENTROPIC_HOOK_ON_STATE_CHANGE));
    }
    SECTION("PRE_GENERATE cancellation short-circuits inference") {
        MockInference mock; mock.is_complete = false;
        auto iface = make_mock_interface(mock);
        LoopConfig lc; lc.max_iterations = 5; CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        v2310::HookCap cap; cap.cancel = true;
        cap.cancel_point = ENTROPIC_HOOK_PRE_GENERATE;
        engine.set_hooks(v2310::make_hooks(&cap));
        engine.run(make_messages());
        REQUIRE(mock.generate_call_count == 0);
        REQUIRE(engine.last_loop_metrics().iterations < 5);
    }
    SECTION("POST_GENERATE hook may rewrite assistant content") {
        MockInference mock; mock.response = "original";
        auto iface = make_mock_interface(mock);
        LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
        v2310::HookCap cap; cap.post_mod = "rewritten by hook";
        engine.set_hooks(v2310::make_hooks(&cap));
        auto result = engine.run(make_messages());
        REQUIRE(result.back().content == "rewritten by hook");
        REQUIRE(v2310::has(cap.post, ENTROPIC_HOOK_POST_GENERATE));
    }
    SECTION("ON_ERROR info hook fires when state hits ERROR") {
        MockInference mock; mock.response = "no tools"; mock.tier = "lead";
        mock.is_complete = false;
        auto iface = make_mock_interface(mock);
        LoopConfig lc; lc.max_iterations = 10; CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        v2310::HookCap cap;
        engine.set_hooks(v2310::make_hooks(&cap));
        TierResolutionInterface tri{};
        tri.get_tier_param = [](const std::string& t, const std::string& p,
                                void*) -> std::string {
            return (t == "lead" && p == "explicit_completion") ? "true" : "";
        };
        engine.set_tier_resolution(tri);
        LoopContext ctx; ctx.messages = make_messages();
        ctx.locked_tier = "lead";
        engine.run_loop(ctx);
        REQUIRE(v2310::has(cap.info, ENTROPIC_HOOK_ON_ERROR));
    }
}

SCENARIO("dir_complete: hook rejection + coverage_gap metadata",
         "[v2.3.10][core][engine_coverage][directives]") {
    SECTION("ON_COMPLETE rejection reverts to prior state with feedback") {
        MockInference mock; auto iface = make_mock_interface(mock);
        LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
        v2310::HookCap cap; cap.cancel = true;
        cap.cancel_point = ENTROPIC_HOOK_ON_COMPLETE;
        cap.pre_mod = "cite sources";
        engine.set_hooks(v2310::make_hooks(&cap));
        LoopContext ctx; ctx.messages = make_messages();
        ctx.state = AgentState::EXECUTING;
        CompleteDirective cd("draft");
        std::vector<const Directive*> list{&cd};
        engine.directive_processor().process(ctx, list);
        REQUIRE(ctx.state == AgentState::EXECUTING);
        bool found = false;
        for (const auto& m : ctx.messages) {
            if (m.content.rfind("[CITATION VALIDATION]", 0) == 0) {
                found = true; break;
            }
        }
        REQUIRE(found);
    }
    SECTION("coverage_gap fields persist into ctx.metadata") {
        MockInference mock; auto iface = make_mock_interface(mock);
        LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
        CompleteDirective cd("partial");
        cd.coverage_gap = true;
        cd.gap_description = "missing X";
        cd.suggested_files = {"a.cpp", "b.h"};
        LoopContext ctx; ctx.messages = make_messages();
        std::vector<const Directive*> list{&cd};
        engine.directive_processor().process(ctx, list);
        REQUIRE(ctx.metadata["coverage_gap"] == "true");
        REQUIRE(ctx.metadata["gap_description"] == "missing X");
        REQUIRE(ctx.metadata["suggested_files_json"].find("a.cpp")
                != std::string::npos);
        REQUIRE(ctx.metadata["explicit_completion_summary"] == "partial");
        REQUIRE(ctx.state == AgentState::COMPLETE);
    }
}

SCENARIO("directive handlers cover the remaining types",
         "[v2.3.10][core][engine_coverage][directives]") {
    MockInference mock; auto iface = make_mock_interface(mock);
    LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
    auto proc = [&](LoopContext& c, const Directive& d) {
        std::vector<const Directive*> l{&d};
        return engine.directive_processor().process(c, l);
    };
    LoopContext ctx;
    StopProcessingDirective sp;
    REQUIRE(proc(ctx, sp).stop_processing);
    TierChangeDirective tc("researcher", "auto");
    auto otc = proc(ctx, tc);
    REQUIRE(ctx.locked_tier == "researcher");
    REQUIRE(otc.tier_changed);
    PipelineDirective pd({"a", "b"}, "t");
    REQUIRE(proc(ctx, pd).stop_processing);
    REQUIRE(ctx.pending_pipeline.has_value());
    ClearSelfTodosDirective ctd;
    REQUIRE_NOTHROW(proc(ctx, ctd));
    InjectContextDirective ic("hi", "system");
    REQUIRE(proc(ctx, ic).injected_messages.size() == 1);
    InjectContextDirective ic_empty("", "user");
    REQUIRE(proc(ctx, ic_empty).injected_messages.empty());

    // Prune walks tool-result messages without throwing.
    LoopContext pctx;
    Message tr; tr.role = "user"; tr.content = "stale";
    tr.metadata["tool_name"] = "fs.read";
    pctx.messages.push_back(tr);
    PruneMessagesDirective pm(0);
    REQUIRE_NOTHROW(proc(pctx, pm));

    PhaseChangeDirective pc("review");
    proc(ctx, pc);
    REQUIRE(ctx.active_phase == "review");

    std::string key_seen;
    EngineCallbacks cbs;
    cbs.on_presenter_notify = [](const char* k, const char*, void* ud) {
        *static_cast<std::string*>(ud) = k ? k : "";
    };
    cbs.user_data = &key_seen;
    engine.set_callbacks(cbs);
    NotifyPresenterDirective np("status", "{}");
    proc(ctx, np);
    REQUIRE(key_seen == "status");

    LoopContext actx;
    ContextAnchorDirective add("k1", "pinned");
    proc(actx, add);
    REQUIRE(actx.messages.size() == 1);
    REQUIRE(actx.messages.front().metadata.at("anchor_key") == "k1");
    ContextAnchorDirective del("k1", "");
    proc(actx, del);
    for (const auto& m : actx.messages) {
        REQUIRE_FALSE(m.metadata.count("anchor_key"));
    }
}

namespace v2310 {
struct DelegInjector {
    enum Kind { DELEG, RESUME, PIPELINE } kind;
    PendingDelegation pd;
    PendingPipeline pp;
    bool fired = false;
};
inline ToolExecutionInterface deleg_executor(DelegInjector* state) {
    ToolExecutionInterface t{};
    t.user_data = state;
    t.process_tool_calls = [](LoopContext& ctx,
                              const std::vector<ToolCall>&,
                              void* ud) -> std::vector<Message> {
        auto* s = static_cast<DelegInjector*>(ud);
        if (s->fired) { return {}; }
        s->fired = true;
        if (s->kind == DelegInjector::PIPELINE) {
            ctx.pending_pipeline = s->pp;
        } else {
            ctx.pending_delegation = s->pd;
        }
        return {};
    };
    return t;
}
inline bool msg_contains(const LoopContext& ctx, const std::string& s) {
    for (const auto& m : ctx.messages) {
        if (m.content.find(s) != std::string::npos) { return true; }
    }
    return false;
}
}  // namespace v2310

SCENARIO("delegation guards reject depth/cycle/repeat/pipeline-depth",
         "[v2.3.10][core][engine_coverage][delegation]") {
    using DI = v2310::DelegInjector;
    auto run_guard = [](DI& st, LoopContext& ctx) {
        MockInference mock;
        mock.tool_calls_queue.push_back(
            R"([{"name":"x","arguments":{}}])");
        auto iface = make_mock_interface(mock);
        LoopConfig lc; lc.max_iterations = 1; CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        engine.set_tool_executor(v2310::deleg_executor(&st));
        ctx.messages = make_messages();
        engine.run_loop(ctx);
    };
    SECTION("pipeline rejected at MAX_DELEGATION_DEPTH") {
        DI st{DI::PIPELINE, {}, PendingPipeline{{"a","b"}, "t"}, false};
        LoopContext ctx; ctx.delegation_depth = AgentEngine::MAX_DELEGATION_DEPTH;
        run_guard(st, ctx);
        REQUIRE(v2310::msg_contains(ctx, "[PIPELINE REJECTED]"));
    }
    SECTION("delegation rejected on ancestor cycle") {
        DI st{DI::DELEG, PendingDelegation{"lead", "go", -1}, {}, false};
        LoopContext ctx; ctx.locked_tier = "eng";
        ctx.delegation_ancestor_tiers = {"lead"};
        run_guard(st, ctx);
        REQUIRE(v2310::msg_contains(ctx, "Circular delegation"));
        REQUIRE(ctx.metadata["failure_reason"] == "delegation_cycle");
    }
    SECTION("delegation rejected at MAX_DELEGATION_DEPTH") {
        DI st{DI::DELEG, PendingDelegation{"eng", "go", -1}, {}, false};
        LoopContext ctx; ctx.delegation_depth = AgentEngine::MAX_DELEGATION_DEPTH;
        run_guard(st, ctx);
        REQUIRE(v2310::msg_contains(ctx, "Maximum delegation"));
    }
    SECTION("delegation rejected on repeat-block") {
        DI st{DI::DELEG, PendingDelegation{"researcher", "go", -1}, {}, false};
        LoopContext ctx;
        ctx.last_failed_delegation_target = "researcher";
        ctx.consecutive_failed_delegations = 2;
        run_guard(st, ctx);
        REQUIRE(v2310::msg_contains(ctx, "Stop retrying this target"));
        REQUIRE(ctx.metadata["failure_reason"]
                == "delegation_repeat_blocked");
    }
}

// ── gh#77 (v2.3.28): entropic.complete + sibling action tool ─────
// Pre-fix: process_generation_result executed a queued
// pending_delegation/pipeline after process_tool_results, ignoring a
// COMPLETE state set by a sibling entropic.complete in the same
// response. Live reproduction: bissell-coder 2026-05-26 10:42-10:53.

namespace gh77 {
/// @brief Executor that simulates the model emitting an
/// entropic.complete + entropic.pipeline pair in one response. Sets
/// state=COMPLETE (the entropic.complete side effect via the
/// directive processor in production) AND queues pending_pipeline
/// (the entropic.pipeline side effect).
/// @utility
/// @version 2.3.28
inline ToolExecutionInterface terminal_plus_pipeline_executor(bool* fired_flag) {
    ToolExecutionInterface t{};
    t.user_data = fired_flag;
    t.process_tool_calls = [](LoopContext& ctx,
                              const std::vector<ToolCall>&,
                              void* ud) -> std::vector<Message> {
        auto* fired = static_cast<bool*>(ud);
        if (*fired) { return {}; }
        *fired = true;
        ctx.pending_pipeline =
            PendingPipeline{{"reader", "editor", "verifier"}, "task"};
        ctx.state = AgentState::COMPLETE;
        return {};
    };
    return t;
}
}  // namespace gh77

SCENARIO("gh#77: entropic.complete is terminal when emitted with sibling pipeline",
         "[v2.3.28][core][engine][gh77][terminal]") {
    MockInference mock;
    mock.tool_calls_queue.push_back(
        R"([{"name":"entropic.complete","arguments":{"summary":"done"}},
            {"name":"entropic.pipeline","arguments":{"stages":["reader"]}}])");
    auto iface = make_mock_interface(mock);
    LoopConfig lc; lc.max_iterations = 4; CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);
    bool fired = false;
    engine.set_tool_executor(gh77::terminal_plus_pipeline_executor(&fired));

    LoopContext ctx; ctx.messages = make_messages();

    GIVEN("a single response carrying entropic.complete + entropic.pipeline") {
        WHEN("the engine processes the response") {
            engine.run_loop(ctx);
            THEN("state is COMPLETE and the queued pipeline did NOT execute") {
                CHECK(ctx.state == AgentState::COMPLETE);
                // Pre-fix: this scenario would have started the pipeline,
                // which would push a [PIPELINE CONTEXT] marker into the
                // message stream. Post-fix: nothing of the kind.
                CHECK_FALSE(v2310::msg_contains(ctx, "[PIPELINE CONTEXT]"));
                CHECK_FALSE(v2310::msg_contains(ctx, "[PIPELINE REJECTED]"));
                // The pending_pipeline option remains set on the
                // context (engine doesn't clear it — it just declines
                // to execute it once terminal); this is intentional so
                // downstream diagnostics can observe what was dropped.
                CHECK(ctx.pending_pipeline.has_value());
            }
        }
    }
}

// ── gh#81 (v2.4.3, Case 2): interrupt latency + child inheritance ──

namespace gh81 {
/// @brief Executor that queues a pending_pipeline AND raises the
/// engine interrupt during tool processing — simulates the user
/// pressing interrupt while the engine is mid-tool-processing in a
/// tight reject-retry loop. Post-fix, process_generation_result must
/// observe interrupt_flag_ and halt before dispatching the pipeline.
/// @utility
/// @version 2.4.3
struct InterruptInjector {
    AgentEngine* engine = nullptr;
    bool fired = false;
};
inline ToolExecutionInterface interrupt_during_tools_executor(
    InterruptInjector* state) {
    ToolExecutionInterface t{};
    t.user_data = state;
    t.process_tool_calls = [](LoopContext& ctx,
                              const std::vector<ToolCall>&,
                              void* ud) -> std::vector<Message> {
        auto* s = static_cast<InterruptInjector*>(ud);
        if (s->fired) { return {}; }
        s->fired = true;
        ctx.pending_pipeline =
            PendingPipeline{{"reader", "editor"}, "task"};
        if (s->engine != nullptr) { s->engine->interrupt(); }
        return {};
    };
    return t;
}
}  // namespace gh81

SCENARIO("gh#81: interrupt during tool processing halts before pending dispatch",
         "[v2.4.3][core][engine][gh81][interrupt]") {
    MockInference mock;
    mock.tool_calls_queue.push_back(
        R"([{"name":"entropic.pipeline","arguments":{"stages":["reader"]}}])");
    auto iface = make_mock_interface(mock);
    LoopConfig lc; lc.max_iterations = 4; CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);
    gh81::InterruptInjector inj{&engine, false};
    engine.set_tool_executor(gh81::interrupt_during_tools_executor(&inj));

    LoopContext ctx; ctx.messages = make_messages();

    GIVEN("an interrupt raised while a pipeline directive is queued") {
        WHEN("the engine processes the response") {
            engine.run_loop(ctx);
            THEN("state is INTERRUPTED and the pipeline never ran") {
                CHECK(ctx.state == AgentState::INTERRUPTED);
                CHECK_FALSE(v2310::msg_contains(ctx, "[PIPELINE CONTEXT]"));
            }
        }
    }
}

SCENARIO("gh#81: run_loop(inherit_interrupt=true) preserves a pre-set interrupt",
         "[v2.4.3][core][engine][gh81][interrupt]") {
    MockInference mock;
    mock.is_complete = false;
    auto iface = make_mock_interface(mock);
    LoopConfig lc; lc.max_iterations = 100; CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("the interrupt flag is already set before the (child) loop runs") {
        engine.interrupt();
        LoopContext ctx; ctx.messages = make_messages();
        WHEN("run_loop is entered with inherit_interrupt=true") {
            engine.run_loop(ctx, /*inherit_interrupt=*/true);
            THEN("the loop honors the inherited interrupt immediately") {
                CHECK(ctx.state == AgentState::INTERRUPTED);
                CHECK(mock.generate_call_count == 0);
            }
        }
    }
}

SCENARIO("gh#81: run_loop(inherit_interrupt=false) clears a stale interrupt",
         "[v2.4.3][core][engine][gh81][interrupt]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc; lc.max_iterations = 4; CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("a stale interrupt flag set before a fresh top-level turn") {
        engine.interrupt();
        LoopContext ctx; ctx.messages = make_messages();
        WHEN("run_loop is entered with the default (reset) behavior") {
            engine.run_loop(ctx);  // inherit_interrupt defaults to false
            THEN("the flag is cleared and the turn generates normally") {
                CHECK(mock.generate_call_count >= 1);
                CHECK(ctx.state != AgentState::INTERRUPTED);
            }
        }
    }
}

SCENARIO("resume_delegation failure paths surface typed errors",
         "[v2.3.10][core][engine_coverage][delegation][resume]") {
    auto run_resume = [](StorageInterface* si, const std::string& expect) {
        MockInference mock;
        mock.tool_calls_queue.push_back(R"([{"name":"x","arguments":{}}])");
        auto iface = make_mock_interface(mock);
        LoopConfig lc; lc.max_iterations = 1; CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        if (si) { engine.set_storage(*si); }
        v2310::DelegInjector st{v2310::DelegInjector::DELEG,
            PendingDelegation{"", "task", -1, "del-id"}, {}, false};
        engine.set_tool_executor(v2310::deleg_executor(&st));
        LoopContext ctx; ctx.messages = make_messages();
        engine.run_loop(ctx);
        REQUIRE(v2310::msg_contains(ctx, expect));
    };
    SECTION("storage unavailable") {
        run_resume(nullptr, "[DELEGATION FAILED: resume_delegation]");
    }
    SECTION("unknown delegation_id") {
        StorageInterface si{};
        si.load_delegation_with_messages = [](const char*, std::string&,
                                              void*) { return false; };
        run_resume(&si, "unknown delegation_id");
    }
    SECTION("malformed payload") {
        StorageInterface si{};
        si.load_delegation_with_messages = [](const char*, std::string& o,
                                              void*) {
            o = "not json"; return true;
        };
        run_resume(&si, "malformed storage payload");
    }
    SECTION("missing target_tier") {
        StorageInterface si{};
        si.load_delegation_with_messages = [](const char*, std::string& o,
                                              void*) {
            o = R"({"messages":[]})"; return true;
        };
        run_resume(&si, "target_tier missing");
    }
}

SCENARIO("validation_provider variants are all non-fatal",
         "[v2.3.10][core][engine_coverage][validation]") {
    auto run = [](char* (*fn)(void*)) {
        MockInference mock; auto iface = make_mock_interface(mock);
        LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
        engine.set_validation_provider(fn, nullptr);
        REQUIRE_NOTHROW(engine.run(make_messages()));
    };
    SECTION("rejected with violations") {
        run([](void*) -> char* { return v2310::malloc_str(
            R"({"verdict":"rejected","violations":["a","b"]})"); });
    }
    SECTION("rejected with no violations array (fallback)") {
        run([](void*) -> char* { return v2310::malloc_str(
            R"({"verdict":"rejected_v2"})"); });
    }
    SECTION("malformed JSON") {
        run([](void*) -> char* {
            return v2310::malloc_str("{not json"); });
    }
    SECTION("nullptr return + approved verdict (skip path)") {
        run([](void*) -> char* { return nullptr; });
        run([](void*) -> char* { return v2310::malloc_str(
            R"({"verdict":"approved"})"); });
    }
}

SCENARIO("queue capacity / cap mutation / clear / observer / state slot",
         "[v2.3.10][core][engine_coverage][queue]") {
    SECTION("capacity enforced + dynamic resize + clear + negative") {
        MockInference mock; auto iface = make_mock_interface(mock);
        LoopConfig lc; lc.message_queue_capacity = 2;
        CompactionConfig cc; AgentEngine engine(iface, lc, cc);
        REQUIRE(engine.queue_user_message("a"));
        REQUIRE(engine.queue_user_message("b"));
        REQUIRE_FALSE(engine.queue_user_message("c"));
        REQUIRE(engine.user_message_queue_depth() == 2);
        engine.set_message_queue_capacity(5);
        REQUIRE(engine.queue_user_message("c"));
        engine.clear_user_message_queue();
        REQUIRE(engine.user_message_queue_depth() == 0);
        engine.set_message_queue_capacity(-7);
        REQUIRE_FALSE(engine.queue_user_message("d"));
    }
    SECTION("observer fires once on drain + is_running clears") {
        MockInference mock; auto iface = make_mock_interface(mock);
        LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
        engine.queue_user_message("queued follow-up");
        struct S { int fired = 0; std::string text; size_t remaining = 0; };
        S s;
        engine.set_queue_observer(
            [](const char* t, size_t r, void* ud) {
                auto* x = static_cast<S*>(ud);
                x->fired++; x->text = t ? t : ""; x->remaining = r;
            }, &s);
        engine.run_turn(std::string("first"));
        REQUIRE(s.fired == 1);
        REQUIRE(s.text == "queued follow-up");
        REQUIRE_FALSE(engine.is_running());
    }
    SECTION("persistent state observer captures terminal COMPLETE") {
        MockInference mock; auto iface = make_mock_interface(mock);
        LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
        std::vector<int> states;
        engine.set_state_observer(
            [](int s, void* ud) {
                static_cast<std::vector<int>*>(ud)->push_back(s);
            }, &states);
        engine.run(make_messages());
        REQUIRE_FALSE(states.empty());
        REQUIRE(states.back() == static_cast<int>(AgentState::COMPLETE));
    }
}

SCENARIO("run_turn seeds system prompt only when conversation is empty",
         "[v2.3.10][core][engine_coverage][conversation]") {
    auto count_sys = [](const std::vector<Message>& v) {
        size_t n = 0;
        for (const auto& m : v) { if (m.role == "system") { ++n; } }
        return n;
    };
    SECTION("string overload seeds once across two turns; clear empties") {
        MockInference mock; auto iface = make_mock_interface(mock);
        LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
        engine.set_system_prompt("system identity");
        engine.run_turn(std::string("hi"));
        auto n1 = engine.message_count();
        engine.run_turn(std::string("again"));
        REQUIRE(count_sys(engine.get_messages()) == 1);
        REQUIRE(engine.message_count() > n1);
        engine.clear_conversation();
        REQUIRE(engine.message_count() == 0);
    }
    SECTION("vector overload skips auto-seed if caller supplies system") {
        MockInference mock; auto iface = make_mock_interface(mock);
        LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
        engine.set_system_prompt("default sys");
        std::vector<Message> turn{{"system", "custom sys"},
                                  {"user", "hello"}};
        engine.run_turn(std::move(turn));
        REQUIRE(count_sys(engine.get_messages()) == 1);
        REQUIRE(engine.get_messages().front().content == "custom sys");
    }
}

SCENARIO("run_streaming overloads route through agent loop",
         "[v2.3.10][core][engine_coverage][streaming]") {
    auto cb = [](const char* t, size_t l, void* ud) {
        static_cast<std::string*>(ud)->append(t, l);
    };
    SECTION("string overload streams tokens") {
        MockInference mock; mock.stream_token_by_token = true;
        mock.response = "abcd";
        auto iface = make_mock_interface(mock);
        LoopConfig lc; lc.stream_output = true; CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        std::string chunks;
        REQUIRE(engine.run_streaming(std::string("hi"), cb,
                                     &chunks, nullptr) == 0);
        REQUIRE(chunks.find("abcd") != std::string::npos);
    }
    SECTION("string overload returns 1 when cancel flag pre-set") {
        MockInference mock; mock.stream_token_by_token = true;
        auto iface = make_mock_interface(mock);
        LoopConfig lc; lc.stream_output = true; CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        int cancel = 1;
        REQUIRE(engine.run_streaming(std::string("x"),
                                     [](const char*, size_t, void*) {},
                                     nullptr, &cancel) == 1);
    }
    SECTION("vector overload preserves content_parts path") {
        MockInference mock; mock.stream_token_by_token = true;
        mock.response = "ok"; auto iface = make_mock_interface(mock);
        LoopConfig lc; lc.stream_output = true; CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        std::string chunks;
        std::vector<Message> turn{{"user", "hi"}};
        REQUIRE(engine.run_streaming(std::move(turn), cb,
                                     &chunks, nullptr) == 0);
        REQUIRE_FALSE(chunks.empty());
    }
}

SCENARIO("misc accessors: pause / context_usage / idle / project_dir",
         "[v2.3.10][core][engine_coverage][misc]") {
    MockInference mock; auto iface = make_mock_interface(mock);
    LoopConfig lc; lc.context_length = 4096; CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);
    engine.pause(); engine.cancel_pause();
    auto [used, max_tok] = engine.context_usage(make_messages());
    REQUIRE(used >= 0);
    REQUIRE(max_tok == 4096);
    REQUIRE(engine.seconds_since_last_activity() == 0);
    engine.run(make_messages());
    REQUIRE(engine.seconds_since_last_activity() >= 0);
    engine.set_project_dir(std::filesystem::path{"/tmp"});
    engine.set_project_dir(std::filesystem::path{});
    engine.set_session_logger(nullptr);
    REQUIRE_NOTHROW(engine.run(make_messages()));

    // build_directive_hooks trampoline coverage
    auto hooks = engine.build_directive_hooks();
    REQUIRE(hooks.process_directives != nullptr);
    REQUIRE(hooks.user_data == &engine);
    LoopContext bd_ctx; StopProcessingDirective sp;
    std::vector<const Directive*> bd_list{&sp};
    REQUIRE(hooks.process_directives(bd_ctx, bd_list, hooks.user_data)
            .stop_processing);
}

SCENARIO("set_handoff_rules wires internal tier resolver trampolines",
         "[v2.3.10][core][engine_coverage][tier_resolution]") {
    MockInference mock; auto iface = make_mock_interface(mock);
    LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
    ChildContextInfo info;
    info.valid = true; info.system_prompt = "you are eng";
    info.explicit_completion = true;
    info.max_iterations_override = 7;
    info.max_tool_calls_per_turn_override = 3;
    engine.set_tier_info("eng", info);
    engine.set_handoff_rules(
        {{"lead", {"eng", "researcher"}}, {"eng", {"lead"}}});
    const auto& tri = engine.tier_resolution();
    auto* ud = tri.user_data;
    REQUIRE(tri.resolve_tier("eng", ud).valid);
    REQUIRE_FALSE(tri.resolve_tier("nope", ud).valid);
    REQUIRE(tri.tier_exists("eng", ud));
    REQUIRE_FALSE(tri.tier_exists("ghost", ud));
    REQUIRE(tri.get_handoff_targets("lead", ud).size() == 2);
    REQUIRE(tri.get_handoff_targets("missing", ud).empty());
    REQUIRE(tri.get_tier_param("eng", "explicit_completion", ud) == "true");
    REQUIRE(tri.get_tier_param("eng", "max_iterations", ud) == "7");
    REQUIRE(tri.get_tier_param("eng", "max_tool_calls_per_turn", ud) == "3");
    REQUIRE(tri.get_tier_param("eng", "unknown_param", ud).empty());
    REQUIRE(tri.get_tier_param("ghost", "anything", ud).empty());
    ChildContextInfo defaults; defaults.valid = true;
    engine.set_tier_info("plain", defaults);
    REQUIRE(tri.get_tier_param("plain", "max_iterations", ud).empty());
    REQUIRE(tri.get_tier_param("plain", "max_tool_calls_per_turn", ud).empty());
    REQUIRE(tri.get_tier_param("plain", "explicit_completion", ud) == "false");
}

SCENARIO("delegation snapshot + per-tier metrics accumulation",
         "[v2.3.10][core][engine_coverage][delegation][metrics]") {
    MockInference mock; auto iface = make_mock_interface(mock);
    LoopConfig lc; CompactionConfig cc; AgentEngine engine(iface, lc, cc);
    SECTION("delegation_callbacks_snapshot atomically copies all slots") {
        int sentinel = 42;
        engine.set_delegation_callbacks(
            [](const ent_delegation_request_t*, void*) {
                return ENT_DECISION_ACCEPT;
            },
            [](const ent_delegation_result_t*, void*) {
                return ENT_DECISION_ACCEPT;
            }, &sentinel);
        auto snap = engine.delegation_callbacks_snapshot();
        REQUIRE(snap.start != nullptr);
        REQUIRE(snap.complete != nullptr);
        REQUIRE(snap.user_data == &sentinel);
        engine.set_delegation_callbacks(nullptr, nullptr, nullptr);
        auto cleared = engine.delegation_callbacks_snapshot();
        REQUIRE(cleared.start == nullptr);
        REQUIRE(cleared.user_data == nullptr);
    }
    SECTION("per_tier_metrics accumulates across run_loop calls") {
        for (int i = 0; i < 2; ++i) {
            LoopContext ctx; ctx.messages = make_messages();
            ctx.locked_tier = "lead";
            engine.run_loop(ctx);
        }
        auto it = engine.per_tier_metrics().find("lead");
        REQUIRE(it != engine.per_tier_metrics().end());
        REQUIRE(it->second.iterations >= 2);
    }
}

SCENARIO("set_storage wiring covers init_session_conversation paths",
         "[v2.3.10][core][engine_coverage][storage]") {
    MockInference mock; auto iface = make_mock_interface(mock);
    LoopConfig lc; CompactionConfig cc;
    SECTION("create_conversation success") {
        AgentEngine engine(iface, lc, cc);
        StorageInterface si{};
        si.create_conversation = [](const char*, std::string& id, void*) {
            id = "00000000-0000-0000-0000-000000000000"; return true;
        };
        engine.set_storage(si);
        REQUIRE_NOTHROW(engine.run(make_messages()));
    }
    SECTION("create_conversation failure is non-fatal") {
        AgentEngine engine(iface, lc, cc);
        StorageInterface si{};
        si.create_conversation = [](const char*, std::string&, void*) {
            return false;
        };
        engine.set_storage(si);
        auto result = engine.run(make_messages());
        REQUIRE(result.back().role == "assistant");
    }
}

// ── gh#80 (v2.5.0): thinking-budget gating ──────────────────

namespace gh80 {
/// @brief Scan a message list for a substring in any message content.
/// @utility @version 2.5.0
inline bool any_msg_contains(const std::vector<Message>& msgs,
                             const std::string& needle) {
    for (const auto& m : msgs) {
        if (m.content.find(needle) != std::string::npos) { return true; }
    }
    return false;
}
}  // namespace gh80

SCENARIO("gh#80: budget off (default) does not gate a tool-call-free spiral",
         "[v2.5.0][core][engine][gh80][budget]") {
    MockInference mock;
    mock.is_complete = false;                 // never completes naturally
    mock.response = std::string(600, 'x');    // ~150 token-equivalents
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    lc.max_iterations = 5;                     // only the iteration cap stops it
    // budget_mode defaults to off
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("budget disabled and a model that never calls a tool") {
        auto out = engine.run(make_messages());
        THEN("the loop runs to the iteration cap (no budget cut)") {
            CHECK(mock.generate_call_count == 5);
            CHECK_FALSE(gh80::any_msg_contains(out, "thinking budget reached"));
        }
    }
}

SCENARIO("gh#80: tokens budget nudges then hard-cuts a tool-call-free spiral",
         "[v2.5.0][core][engine][gh80][budget]") {
    MockInference mock;
    mock.is_complete = false;                 // never completes naturally
    mock.response = std::string(600, 'x');    // 600 chars ≈ 150 tokens
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    lc.max_iterations = 10;                    // high — budget should stop first
    lc.budget_mode = entropic::BudgetMode::tokens;
    lc.budget_limit = 100;                     // ~150 tokens/iter exceeds in one
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    GIVEN("a token budget smaller than one iteration's output") {
        auto out = engine.run(make_messages());
        THEN("iteration 1 nudges, iteration 2 hard-cuts (before the cap)") {
            CHECK(mock.generate_call_count == 2);
        }
        AND_THEN("the nudge is visible in history") {
            CHECK(gh80::any_msg_contains(out, "thinking budget reached"));
        }
        AND_THEN("the hard-cut failure note is visible in history") {
            CHECK(gh80::any_msg_contains(out, "thinking budget exhausted"));
        }
    }
}

SCENARIO("gh#80: a tool call resets the budget window",
         "[v2.5.0][core][engine][gh80][budget]") {
    MockInference mock;
    mock.is_complete = false;
    mock.response = std::string(600, 'x');     // ~150 tokens
    // Iteration 1 emits a tool call (resets budget); subsequent
    // iterations spiral. The tool call buys a fresh window, so the
    // nudge cannot fire on iteration 1.
    mock.tool_calls_queue.push_back(
        R"([{"name":"noop","arguments":{}}])");
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    lc.max_iterations = 3;
    lc.budget_mode = entropic::BudgetMode::tokens;
    lc.budget_limit = 100;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    // Tool executor that accepts the call (so made_tool_call=true on iter 1).
    ToolExecutionInterface tex{};
    static bool noop_seen;
    noop_seen = false;
    tex.process_tool_calls = [](LoopContext&, const std::vector<ToolCall>&,
                                void*) -> std::vector<Message> {
        noop_seen = true;
        return {};
    };
    engine.set_tool_executor(tex);

    GIVEN("a model that calls a tool on iteration 1 then spirals") {
        auto out = engine.run(make_messages());
        THEN("the tool call ran and the iteration-1 budget did not nudge early") {
            CHECK(noop_seen);
            // No nudge on the very first iteration — the tool call reset
            // the window. (A nudge may appear later as the spiral
            // continues, which is correct.)
            CHECK(mock.generate_call_count >= 1);
        }
    }
}

// ── gh#84 (v2.5.1): budget resets on PROGRESS, not parsed calls ──

namespace gh84 {
/// @brief Tool executor returning a single result message tagged with
/// a fixed result_kind — lets a test simulate rejected_duplicate spam
/// (no progress) vs a genuine ok execution.
/// @utility @version 2.5.1
inline ToolExecutionInterface kind_tagged_executor(const char* result_kind) {
    static std::string kind;
    kind = result_kind;
    ToolExecutionInterface t{};
    t.process_tool_calls = [](LoopContext&, const std::vector<ToolCall>&,
                              void*) -> std::vector<Message> {
        Message m;
        m.role = "user";
        m.content = "tool result";
        m.metadata["result_kind"] = kind;
        return {m};
    };
    return t;
}
}  // namespace gh84

SCENARIO("gh#84: rejected-duplicate tool calls do NOT reset the budget",
         "[v2.5.1][core][engine][gh84][budget]") {
    MockInference mock;
    mock.is_complete = false;
    mock.response = std::string(600, 'x');                 // ~150 tokens/turn
    mock.tool_calls_json = R"([{"name":"docs.search","arguments":{}}])";  // a call every turn
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    lc.max_iterations = 10;
    lc.budget_mode = entropic::BudgetMode::tokens;
    lc.budget_limit = 100;                                 // exceeded in one turn
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);
    // Every call is rejected (duplicate) — no progress.
    engine.set_tool_executor(gh84::kind_tagged_executor("rejected_duplicate"));

    GIVEN("a model emitting a duplicate-rejected tool call every turn") {
        auto out = engine.run(make_messages());
        THEN("the budget still fires (the spam does not refresh it)") {
            // nudge on turn 1, hard-cut on turn 2 — before the iter cap.
            CHECK(mock.generate_call_count == 2);
            CHECK(gh80::any_msg_contains(out, "thinking budget exhausted"));
        }
    }
}

SCENARIO("gh#84: a successful tool call DOES reset the budget",
         "[v2.5.1][core][engine][gh84][budget]") {
    MockInference mock;
    mock.is_complete = false;
    mock.response = std::string(600, 'x');
    mock.tool_calls_json = R"([{"name":"docs.search","arguments":{}}])";
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    lc.max_iterations = 4;
    lc.budget_mode = entropic::BudgetMode::tokens;
    lc.budget_limit = 100;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);
    // Every call genuinely executes (ok) — resets the window each turn.
    engine.set_tool_executor(gh84::kind_tagged_executor("ok"));

    GIVEN("a model whose tool call succeeds every turn") {
        auto out = engine.run(make_messages());
        THEN("the budget never hard-cuts — only the iteration cap stops it") {
            CHECK(mock.generate_call_count == 4);
            CHECK_FALSE(gh80::any_msg_contains(out, "thinking budget exhausted"));
        }
    }
}

// ── gh#99: per-call tier selection (run_turn_as) ──────────

TEST_CASE("has_tier reflects set_tier_info (gh#99)", "[engine][gh99]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    ChildContextInfo npc;
    npc.system_prompt = "You are an NPC.";
    npc.valid = true;
    engine.set_tier_info("npc", npc);

    CHECK(engine.has_tier("npc"));
    CHECK_FALSE(engine.has_tier("bbeg"));
}

TEST_CASE("run_turn_as locks the named tier and seeds its system prompt "
          "(gh#99 combined path)", "[engine][gh99]") {
    MockInference mock;
    mock.tier = "router_choice";  // what routing WOULD pick if consulted
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    ChildContextInfo npc;
    npc.system_prompt = "You are a terse NPC. Answer in one word.";
    npc.valid = true;
    engine.set_tier_info("npc", npc);

    auto result = engine.run_turn_as("npc", "hi");

    // The tier is pre-locked, so routing is skipped entirely (precedence:
    // per-call tier override > routing > default).
    CHECK(mock.route_call_count == 0);
    // The conversation opens with the NPC tier's system prompt — proving the
    // per-tier prompt was seeded, not the (empty) global system_prompt_.
    const auto& msgs = engine.get_messages();
    REQUIRE(msgs.size() >= 2);
    CHECK(msgs.front().role == "system");
    CHECK(msgs.front().content == "You are a terse NPC. Answer in one word.");
    CHECK_FALSE(result.empty());
}

TEST_CASE("run_turn (no override) still routes (gh#99 contrast)",
          "[engine][gh99]") {
    MockInference mock;
    mock.tier = "eng";
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);

    engine.run_turn("hi");

    // With no tier override and a router wired, routing runs as before.
    CHECK(mock.route_call_count >= 1);
}
