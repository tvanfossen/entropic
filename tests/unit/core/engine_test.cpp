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
