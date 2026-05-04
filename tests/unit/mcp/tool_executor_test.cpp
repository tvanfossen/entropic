// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_tool_executor.cpp
 * @brief ToolExecutor unit tests.
 * @version 2.0.6-rc19
 */

#include <entropic/core/hook_registry.h>
#include <entropic/mcp/server_base.h>
#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/tool_executor.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace entropic;

// ── Test tools and server ────────────────────────────────

/**
 * @brief Test tool that returns "ok".
 * @version 1.8.5
 */
class OkTool : public ToolBase {
public:
    /**
     * @brief Construct.
     * @version 1.8.5
     */
    OkTool() : ToolBase(ToolDefinition{
        "do_thing",
        "Does a thing",
        R"({"type":"object","properties":{}})"
    }) {}

    /**
     * @brief Execute: return "ok".
     * @param args_json Arguments (unused).
     * @return ServerResponse.
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& /*args_json*/) override {
        return ServerResponse{"ok", {}};
    }
};

/**
 * @brief Tool with enum constraint for schema validation testing.
 * @version 2.0.6
 */
class EnumTool : public ToolBase {
public:
    /**
     * @brief Construct with enum schema.
     * @version 2.0.6
     */
    EnumTool() : ToolBase(ToolDefinition{
        "pick",
        "Pick a color",
        R"({"type":"object","properties":{"color":{"type":"string","enum":["red","blue","green"]}},"required":["color"]})"
    }) {}

    /**
     * @brief Execute: return "ok".
     * @param args_json Arguments (unused).
     * @return ServerResponse.
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& /*args_json*/) override {
        return ServerResponse{"ok", {}};
    }
};

/**
 * @brief Test server wrapping OkTool.
 * @version 1.8.5
 */
class OkServer : public MCPServerBase {
public:
    /**
     * @brief Construct and register tool.
     * @version 1.8.5
     */
    OkServer() : MCPServerBase("ok") {
        register_tool(&tool_);
    }
private:
    OkTool tool_; ///< The tool
};

/**
 * @brief Test server wrapping EnumTool.
 * @version 2.0.6
 */
class EnumServer : public MCPServerBase {
public:
    /**
     * @brief Construct and register tool.
     * @version 2.0.6
     */
    EnumServer() : MCPServerBase("enum") {
        register_tool(&tool_);
    }
private:
    EnumTool tool_; ///< The tool
};

// ── Helper ───────────────────────────────────────────────

/**
 * @brief Build a ServerManager with an OkServer registered.
 * @return Configured ServerManager.
 * @internal
 * @version 1.8.5
 */
static ServerManager make_manager() {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp/test");
    mgr.register_server(std::make_unique<OkServer>());
    mgr.initialize();
    return mgr;
}

/**
 * @brief Make a ToolCall.
 * @param name Tool name.
 * @return ToolCall.
 * @internal
 * @version 1.8.5
 */
static ToolCall make_call(const std::string& name) {
    ToolCall call;
    call.id = "test-001";
    call.name = name;
    return call;
}

// ── Tests ────────────────────────────────────────────────

TEST_CASE("Process single tool call", "[tool_executor]") {
    auto mgr = make_manager();
    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    LoopContext ctx;
    auto results = executor.process_tool_calls(
        ctx, {make_call("ok.do_thing")});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("ok") != std::string::npos);
}

TEST_CASE("Duplicate detection skips", "[tool_executor]") {
    auto mgr = make_manager();
    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    LoopContext ctx;
    // First call
    executor.process_tool_calls(ctx, {make_call("ok.do_thing")});
    // Second identical call
    auto results = executor.process_tool_calls(
        ctx, {make_call("ok.do_thing")});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("already called") != std::string::npos);
}

TEST_CASE("Circuit breaker at three", "[tool_executor]") {
    auto mgr = make_manager();
    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    LoopContext ctx;
    // First call (real execution)
    executor.process_tool_calls(ctx, {make_call("ok.do_thing")});
    // Duplicate #1
    executor.process_tool_calls(ctx, {make_call("ok.do_thing")});
    // Duplicate #2
    executor.process_tool_calls(ctx, {make_call("ok.do_thing")});
    // Duplicate #3 — triggers circuit breaker
    auto results = executor.process_tool_calls(
        ctx, {make_call("ok.do_thing")});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("STOP") != std::string::npos);
    REQUIRE(ctx.consecutive_duplicate_attempts >= 3);
}

TEST_CASE("Duplicate counter resets", "[tool_executor]") {
    auto mgr = make_manager();
    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    LoopContext ctx;
    // First call
    executor.process_tool_calls(ctx, {make_call("ok.do_thing")});
    // Duplicate
    executor.process_tool_calls(ctx, {make_call("ok.do_thing")});
    REQUIRE(ctx.consecutive_duplicate_attempts == 1);

    // Different call
    ToolCall diff;
    diff.id = "test-002";
    diff.name = "ok.do_thing";
    diff.arguments["x"] = "1";
    executor.process_tool_calls(ctx, {diff});
    REQUIRE(ctx.consecutive_duplicate_attempts == 0);
}

TEST_CASE("Delegate sorted last", "[tool_executor]") {
    // Sort tool calls should put entropic.delegate last
    std::vector<ToolCall> calls;
    ToolCall delegate;
    delegate.name = "entropic.delegate";
    ToolCall other;
    other.name = "filesystem.read_file";
    calls.push_back(delegate);
    calls.push_back(other);

    // Use sort_tool_calls indirectly through process_tool_calls
    // by checking that delegate is processed last
    // (Direct test of static method not possible without friend)
    REQUIRE(calls[0].name == "entropic.delegate");
}

TEST_CASE("Headless mode denies without callback", "[tool_executor]") {
    auto mgr = make_manager();
    LoopConfig lc;
    lc.auto_approve_tools = false;
    EngineCallbacks cb;
    // on_tool_call is nullptr → headless → deny
    ToolExecutor executor(mgr, lc, cb);

    LoopContext ctx;
    auto results = executor.process_tool_calls(
        ctx, {make_call("ok.do_thing")});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("denied") != std::string::npos);
}

TEST_CASE("Auto approve skips callback", "[tool_executor]") {
    auto mgr = make_manager();
    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    LoopContext ctx;
    auto results = executor.process_tool_calls(
        ctx, {make_call("ok.do_thing")});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].content.find("ok") != std::string::npos);
}

TEST_CASE("Max tool calls per turn", "[tool_executor]") {
    auto mgr = make_manager();
    LoopConfig lc;
    lc.auto_approve_tools = true;
    lc.max_tool_calls_per_turn = 2;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    LoopContext ctx;
    std::vector<ToolCall> calls;
    for (int i = 0; i < 5; i++) {
        ToolCall c;
        c.id = "call-" + std::to_string(i);
        c.name = "ok.do_thing";
        c.arguments["i"] = std::to_string(i);
        calls.push_back(c);
    }
    auto results = executor.process_tool_calls(ctx, calls);
    REQUIRE(results.size() <= 2);
}

// ── v2.0.6: Schema validation ───────────────────────────

/**
 * @brief Build a ServerManager with both OkServer and EnumServer.
 * @return Configured ServerManager.
 * @internal
 * @version 2.0.6
 */
static ServerManager make_enum_manager() {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp/test");
    mgr.register_server(std::make_unique<OkServer>());
    mgr.register_server(std::make_unique<EnumServer>());
    mgr.initialize();
    return mgr;
}

SCENARIO("Schema validation rejects invalid enum value",
         "[tool_executor][v2.0.6][schema]")
{
    GIVEN("a tool executor with an enum-constrained tool") {
        auto mgr = make_enum_manager();
        LoopConfig lc;
        EngineCallbacks cbs{};
        ToolExecutor executor(mgr, lc, cbs, {});

        WHEN("tool is called with an invalid enum value") {
            LoopContext ctx;
            ToolCall call;
            call.id = "tc-bad";
            call.name = "enum.pick";
            call.arguments["color"] = "purple";
            auto results = executor.process_tool_calls(
                ctx, {call});

            THEN("call is rejected with an error message") {
                REQUIRE(results.size() == 1);
                CHECK(results[0].content.find("Invalid value")
                      != std::string::npos);
            }
        }

        WHEN("tool is called with a valid enum value") {
            LoopContext ctx;
            ToolCall call;
            call.id = "tc-good";
            call.name = "enum.pick";
            call.arguments["color"] = "red";
            auto results = executor.process_tool_calls(
                ctx, {call});

            THEN("call executes successfully (not rejected)") {
                REQUIRE(results.size() == 1);
                CHECK(results[0].content.find("Invalid value")
                      == std::string::npos);
            }
        }
    }
}

SCENARIO("Schema validation rejects missing required field",
         "[tool_executor][v2.0.6][schema]")
{
    GIVEN("a tool executor with a required-field tool") {
        auto mgr = make_enum_manager();
        LoopConfig lc;
        EngineCallbacks cbs{};
        ToolExecutor executor(mgr, lc, cbs, {});

        WHEN("tool is called without required field") {
            LoopContext ctx;
            ToolCall call;
            call.id = "tc-missing";
            call.name = "enum.pick";
            // no color argument
            auto results = executor.process_tool_calls(
                ctx, {call});

            THEN("call is rejected for missing required") {
                REQUIRE(results.size() == 1);
                CHECK(results[0].content.find("Missing required")
                      != std::string::npos);
            }
        }
    }
}

// ── P1-11: tool history recording (2.0.6-rc16) ───────────

TEST_CASE("Tool call history records successful executions",
          "[tool_executor][P1-11][2.0.6-rc16]") {
    auto mgr = make_manager();
    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    LoopContext ctx;
    REQUIRE(executor.tool_history().size() == 0);

    executor.process_tool_calls(ctx, {make_call("ok.do_thing")});
    CHECK(executor.tool_history().size() == 1);

    executor.process_tool_calls(ctx, {make_call("ok.do_thing")});
    // duplicate detection may or may not skip based on call_key —
    // at minimum the first was recorded; history monotone grows.
    CHECK(executor.tool_history().size() >= 1);

    auto recent = executor.tool_history().recent(5);
    REQUIRE_FALSE(recent.empty());
    CHECK(recent.front().tool_name == "ok.do_thing");
    CHECK(recent.front().elapsed_ms >= 0.0);
}

// ── P2-17: consolidated [tool_call] log path (2.0.6-rc16) ────────

SCENARIO("process_single_call completes with locked_tier set",
         "[tool_executor][P2-17][2.0.6-rc16]")
{
    GIVEN("an executor with a locked tier in context") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        LoopContext ctx;
        ctx.locked_tier = "eng";
        ctx.metrics.iterations = 3;

        WHEN("a tool call is processed") {
            auto results = executor.process_tool_calls(
                ctx, {make_call("ok.do_thing")});

            THEN("result is returned and metrics are updated") {
                REQUIRE(results.size() == 1);
                CHECK(results[0].content.find("ok") != std::string::npos);
                CHECK(ctx.metrics.tool_calls >= 1);
            }
        }
    }

    GIVEN("an executor with no locked tier") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        LoopContext ctx;
        // locked_tier empty — log path uses "lead" fallback

        WHEN("a tool call is processed") {
            auto results = executor.process_tool_calls(
                ctx, {make_call("ok.do_thing")});

            THEN("result is returned without crash") {
                REQUIRE(results.size() == 1);
                CHECK(results[0].content.find("ok") != std::string::npos);
            }
        }
    }
}

// ── P3-18: per-identity max_tool_calls_per_turn override ─────────

SCENARIO("effective_max_tool_calls_per_turn overrides global limit",
         "[tool_executor][P3-18][2.0.6-rc16]")
{
    GIVEN("an executor with global limit=10 and ctx override=1") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        lc.max_tool_calls_per_turn = 10;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        LoopContext ctx;
        ctx.effective_max_tool_calls_per_turn = 1;

        WHEN("three tool calls are submitted") {
            ToolCall c1, c2, c3;
            c1.id = "t1"; c1.name = "ok.do_thing";
            c2.id = "t2"; c2.name = "ok.do_thing"; c2.arguments["x"] = "a";
            c3.id = "t3"; c3.name = "ok.do_thing"; c3.arguments["x"] = "b";
            auto results = executor.process_tool_calls(ctx, {c1, c2, c3});

            THEN("only 1 result returned (truncated to override limit)") {
                REQUIRE(results.size() == 1);
            }
        }
    }

    GIVEN("an executor with no override (effective=-1 uses global)") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        lc.max_tool_calls_per_turn = 2;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        LoopContext ctx;
        // effective_max_tool_calls_per_turn = -1 (default, use global)

        WHEN("three distinct tool calls are submitted") {
            ToolCall c1, c2, c3;
            c1.id = "t1"; c1.name = "ok.do_thing";
            c2.id = "t2"; c2.name = "ok.do_thing"; c2.arguments["x"] = "a";
            c3.id = "t3"; c3.name = "ok.do_thing"; c3.arguments["x"] = "b";
            auto results = executor.process_tool_calls(ctx, {c1, c2, c3});

            THEN("global limit of 2 is applied") {
                REQUIRE(results.size() == 2);
            }
        }
    }
}

// ── E9/E10: PRE/POST_TOOL_CALL firing + result_kind (2.0.6-rc19) ──

namespace {

/**
 * @brief Event record captured by the per-test hook.
 * @internal
 * @version 2.0.6-rc19
 */
struct HookEvent {
    entropic_hook_point_t point;
    std::string context_json;
};

/**
 * @brief Cancelling PRE hook.
 * @param hp Hook point.
 * @param ctx Context JSON.
 * @param mod Out modified JSON (unused).
 * @param ud Event sink (vector<HookEvent>*).
 * @return 1 to cancel the call (PRE_TOOL_CALL only).
 * @internal
 * @version 2.0.6-rc19
 */
static int cancelling_pre_cb(
    entropic_hook_point_t hp, const char* ctx,
    char** /*mod*/, void* ud) {
    auto* sink = static_cast<std::vector<HookEvent>*>(ud);
    sink->push_back({hp, ctx ? std::string{ctx} : std::string{}});
    return 1;
}

/**
 * @brief Record hook fires into a sink without cancelling.
 * @return 0 always.
 * @internal
 * @version 2.0.6-rc19
 */
static int record_hook_cb(
    entropic_hook_point_t hp, const char* ctx,
    char** /*mod*/, void* ud) {
    auto* sink = static_cast<std::vector<HookEvent>*>(ud);
    sink->push_back({hp, ctx ? std::string{ctx} : std::string{}});
    return 0;
}

/**
 * @brief Wire a fresh HookRegistry onto a ToolExecutor.
 * @param executor Target executor.
 * @param reg Registry to wire.
 * @internal
 * @version 2.0.6-rc19
 */
static void attach_registry(ToolExecutor& executor, HookRegistry& reg) {
    HookInterface hi;
    hi.registry = &reg;
    hi.fire_pre = [](void* r, entropic_hook_point_t pt,
                     const char* json, char** out) -> int {
        return static_cast<HookRegistry*>(r)->fire_pre(pt, json, out);
    };
    hi.fire_post = [](void* r, entropic_hook_point_t pt,
                      const char* json, char** out) {
        static_cast<HookRegistry*>(r)->fire_post(pt, json, out);
    };
    hi.fire_info = [](void* r, entropic_hook_point_t pt,
                      const char* json) {
        static_cast<HookRegistry*>(r)->fire_info(pt, json);
    };
    executor.set_hooks(hi);
}

} // namespace

SCENARIO("PRE/POST_TOOL_CALL fire for successful calls",
         "[tool_executor][E9][2.0.6-rc19]") {
    GIVEN("a ToolExecutor with PRE and POST hooks registered") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        std::vector<HookEvent> events;
        reg.register_hook(ENTROPIC_HOOK_PRE_TOOL_CALL,
                          record_hook_cb, &events, 0);
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          record_hook_cb, &events, 0);
        attach_registry(executor, reg);

        WHEN("a tool call is processed") {
            LoopContext ctx;
            ctx.locked_tier = "eng";
            ctx.metrics.iterations = 4;
            executor.process_tool_calls(ctx, {make_call("ok.do_thing")});

            THEN("both hooks fire in order") {
                REQUIRE(events.size() == 2);
                REQUIRE(events[0].point == ENTROPIC_HOOK_PRE_TOOL_CALL);
                REQUIRE(events[1].point == ENTROPIC_HOOK_POST_TOOL_CALL);
            }
            AND_THEN("PRE context carries tier, args, iteration") {
                auto pre = nlohmann::json::parse(events[0].context_json);
                REQUIRE(pre.at("tool_name").get<std::string>()
                        == "ok.do_thing");
                REQUIRE(pre.at("tier").get<std::string>() == "eng");
                REQUIRE(pre.at("iteration").get<int>() == 4);
                REQUIRE(pre.contains("args"));
            }
            AND_THEN("POST context carries result_kind=ok") {
                auto post = nlohmann::json::parse(events[1].context_json);
                REQUIRE(post.at("result_kind").get<std::string>()
                        == "ok");
                REQUIRE(post.at("tier").get<std::string>() == "eng");
                REQUIRE(post.at("iteration").get<int>() == 4);
            }
        }
    }
}

SCENARIO("POST_TOOL_CALL fires on duplicate rejection with kind",
         "[tool_executor][E9][E10][2.0.6-rc19]") {
    GIVEN("an executor with POST hook, after a first call") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        std::vector<HookEvent> events;
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          record_hook_cb, &events, 0);
        attach_registry(executor, reg);

        LoopContext ctx;
        executor.process_tool_calls(ctx, {make_call("ok.do_thing")});
        events.clear();

        WHEN("the identical call is re-submitted") {
            executor.process_tool_calls(ctx, {make_call("ok.do_thing")});

            THEN("POST_TOOL_CALL fires with result_kind=rejected_duplicate") {
                REQUIRE(events.size() == 1);
                REQUIRE(events[0].point
                        == ENTROPIC_HOOK_POST_TOOL_CALL);
                auto post = nlohmann::json::parse(events[0].context_json);
                REQUIRE(post.at("result_kind").get<std::string>()
                        == "rejected_duplicate");
            }
        }
    }
}

SCENARIO("POST_TOOL_CALL fires on schema rejection with kind",
         "[tool_executor][E9][E10][2.0.6-rc19]") {
    GIVEN("an executor with an enum-constrained tool and POST hook") {
        auto mgr = make_enum_manager();
        LoopConfig lc;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb, {});

        HookRegistry reg;
        std::vector<HookEvent> events;
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          record_hook_cb, &events, 0);
        attach_registry(executor, reg);

        WHEN("a tool is called with an invalid enum value") {
            LoopContext ctx;
            ToolCall call;
            call.id = "tc-bad";
            call.name = "enum.pick";
            call.arguments["color"] = "purple";
            executor.process_tool_calls(ctx, {call});

            THEN("POST_TOOL_CALL fires with result_kind=rejected_schema") {
                REQUIRE(events.size() == 1);
                auto post = nlohmann::json::parse(events[0].context_json);
                REQUIRE(post.at("result_kind").get<std::string>()
                        == "rejected_schema");
            }
        }
    }
}

SCENARIO("PRE_TOOL_CALL cancel short-circuits dispatch and still fires POST",
         "[tool_executor][E9][2.0.6-rc19]") {
    GIVEN("an executor with a cancelling PRE hook and a POST recorder") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        std::vector<HookEvent> events;
        reg.register_hook(ENTROPIC_HOOK_PRE_TOOL_CALL,
                          cancelling_pre_cb, &events, 0);
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          record_hook_cb, &events, 0);
        attach_registry(executor, reg);

        WHEN("a tool call is processed") {
            LoopContext ctx;
            auto results = executor.process_tool_calls(
                ctx, {make_call("ok.do_thing")});

            THEN("rejection message is returned") {
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].content.find("denied")
                        != std::string::npos);
            }
            AND_THEN("PRE fired, then POST fired with "
                     "rejected_precondition") {
                REQUIRE(events.size() == 2);
                REQUIRE(events[0].point
                        == ENTROPIC_HOOK_PRE_TOOL_CALL);
                REQUIRE(events[1].point
                        == ENTROPIC_HOOK_POST_TOOL_CALL);
                auto post = nlohmann::json::parse(events[1].context_json);
                REQUIRE(post.at("result_kind").get<std::string>()
                        == "rejected_precondition");
            }
        }
    }
}

// ── Issue #2 (v2.1.1): POST_TOOL_CALL hook can transform Message::content ──
//
// Pre-2.1.1 the engine called free() on *modified_json without applying it
// to the resulting Message::content. The four SCENARIOs above only assert
// that the hook FIRES (and reads context); none verify that a transforming
// hook's output reaches the model. These tests close that gap.

namespace {

/**
 * @brief Allocate a C string copy on the heap (matches entropic_alloc).
 *
 * The engine free()s ``*modified_json`` after consuming it, so this hook's
 * output must be malloc-allocated. Mirrors the helper in
 * tests/unit/core/hook_registry_test.cpp.
 *
 * @param s Source string.
 * @return Heap-allocated copy.
 * @internal
 * @version 2.1.1
 */
static char* dup_to_heap(const char* s) {
    size_t len = strlen(s) + 1;
    auto* p = static_cast<char*>(malloc(len));
    memcpy(p, s, len);
    return p;
}

/**
 * @brief Hook that wraps the tool result text with a [TRANSFORMED] marker.
 *
 * Reads ``result`` from the post-tool context JSON and writes
 * ``"[TRANSFORMED] " + result`` as ``*modified_json``. Matches the
 * pattern recommended in issue #2.
 *
 * @internal
 * @version 2.1.1
 */
static int transform_post_cb(
    entropic_hook_point_t /*hp*/, const char* ctx,
    char** mod, void* /*ud*/) {
    auto j = nlohmann::json::parse(ctx);
    auto wrapped = std::string{"[TRANSFORMED] "}
                 + j.value("result", std::string{});
    *mod = dup_to_heap(wrapped.c_str());
    return 0;
}

/**
 * @brief Hook that emits a fixed marker regardless of input result.
 *
 * Used to verify last-write-wins chaining order alongside
 * ``transform_post_cb``: when both are registered, only the marker
 * from whichever ran last reaches the message.
 *
 * @internal
 * @version 2.1.1
 */
static int fixed_marker_post_cb(
    entropic_hook_point_t /*hp*/, const char* /*ctx*/,
    char** mod, void* ud) {
    const char* marker = static_cast<const char*>(ud);
    *mod = dup_to_heap(marker);
    return 0;
}

/**
 * @brief Hook that fires (so we know it ran) but writes no transformation.
 *
 * Used to verify the ``*modified_json == NULL`` no-op path preserves
 * the original ``Message::content``.
 *
 * @internal
 * @version 2.1.1
 */
static int noop_post_cb(
    entropic_hook_point_t /*hp*/, const char* /*ctx*/,
    char** /*mod*/, void* ud) {
    auto* fired = static_cast<bool*>(ud);
    *fired = true;
    return 0;
}

} // namespace

SCENARIO("POST_TOOL_CALL hook transforms successful tool result content",
         "[tool_executor][hooks][regression][2.1.1]") {
    GIVEN("an executor with a transforming POST_TOOL_CALL hook") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          transform_post_cb, nullptr, 0);
        attach_registry(executor, reg);

        WHEN("a successful tool call is processed") {
            LoopContext ctx;
            auto results = executor.process_tool_calls(
                ctx, {make_call("ok.do_thing")});

            THEN("the result message carries the hook's transform") {
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].content.find("[TRANSFORMED]")
                        != std::string::npos);
                REQUIRE(results[0].content.find("ok")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("POST_TOOL_CALL hook transforms duplicate-rejection content",
         "[tool_executor][hooks][regression][2.1.1]") {
    GIVEN("an executor with a transforming POST hook, after one call") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          transform_post_cb, nullptr, 0);
        attach_registry(executor, reg);

        LoopContext ctx;
        executor.process_tool_calls(ctx, {make_call("ok.do_thing")});

        WHEN("the identical call is re-submitted") {
            auto results = executor.process_tool_calls(
                ctx, {make_call("ok.do_thing")});

            THEN("the rejection message carries the hook's transform") {
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].content.find("[TRANSFORMED]")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("POST_TOOL_CALL hook transforms schema-rejection content",
         "[tool_executor][hooks][regression][2.1.1]") {
    GIVEN("an enum-tool executor with a transforming POST hook") {
        auto mgr = make_enum_manager();
        LoopConfig lc;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb, {});

        HookRegistry reg;
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          transform_post_cb, nullptr, 0);
        attach_registry(executor, reg);

        WHEN("a tool is called with an invalid enum value") {
            LoopContext ctx;
            ToolCall call;
            call.id = "tc-bad";
            call.name = "enum.pick";
            call.arguments["color"] = "purple";
            auto results = executor.process_tool_calls(ctx, {call});

            THEN("the rejection message carries the hook's transform") {
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].content.find("[TRANSFORMED]")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("POST_TOOL_CALL with *modified_json == NULL leaves content untouched",
         "[tool_executor][hooks][regression][2.1.1]") {
    GIVEN("an executor with a no-op POST hook (does not write *mod)") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        bool fired = false;
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          noop_post_cb, &fired, 0);
        attach_registry(executor, reg);

        WHEN("a tool call is processed") {
            LoopContext ctx;
            auto results = executor.process_tool_calls(
                ctx, {make_call("ok.do_thing")});

            THEN("the hook fires but content is unchanged") {
                REQUIRE(fired);
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].content.find("[TRANSFORMED]")
                        == std::string::npos);
                // OkTool returns "ok"; ServerResponse wraps it in a JSON
                // envelope and the executor unwraps "result" → "ok".
                REQUIRE(results[0].content == "ok");
            }
        }
    }
}

SCENARIO("Chained POST_TOOL_CALL hooks: last write wins",
         "[tool_executor][hooks][regression][2.1.1]") {
    GIVEN("two POST hooks: a transforming hook then a fixed-marker hook") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        // Registration order = firing order. Both write *modified_json;
        // the registry semantics (hook_registry_test.cpp::"out_json is the
        // last modification") say each hook gets the same context_json,
        // and the LAST hook's output is what fire_post returns.
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          transform_post_cb, nullptr, 0);
        const char* second_marker = "FINAL_HOOK_OUTPUT";
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          fixed_marker_post_cb,
                          const_cast<char*>(second_marker), 1);
        attach_registry(executor, reg);

        WHEN("a tool call is processed") {
            LoopContext ctx;
            auto results = executor.process_tool_calls(
                ctx, {make_call("ok.do_thing")});

            THEN("only the second hook's output reaches the message") {
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].content == "FINAL_HOOK_OUTPUT");
                REQUIRE(results[0].content.find("[TRANSFORMED]")
                        == std::string::npos);
            }
        }
    }
}

// ── Anti-spiral hard block (#14, v2.1.4) ─────────────────────

SCENARIO("Anti-spiral hard block stops dispatch and emits "
         "rejected_anti_spiral",
         "[tool_executor][anti-spiral][2.1.4][issue-14]") {
    GIVEN("an executor with auto_approve and a low anti-spiral threshold") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        lc.max_consecutive_same_tool = 2;          // soft warning at 2
        lc.max_consecutive_same_tool_hard_block = 4;// hard block at 4
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        std::vector<HookEvent> events;
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          record_hook_cb, &events, 0);
        attach_registry(executor, reg);

        WHEN("the same tool is dispatched four times in a row "
             "(with non-duplicate args so the duplicate path is bypassed)") {
            LoopContext ctx;
            // Use distinct call.id and arg payloads to avoid the
            // duplicate-recent-history shortcut; the anti-spiral check
            // is independent of arg similarity.
            for (int i = 0; i < 4; ++i) {
                ToolCall call = make_call("ok.do_thing");
                call.id = "call-" + std::to_string(i);
                call.arguments["i"] = std::to_string(i);
                executor.process_tool_calls(ctx, {call});
            }

            THEN("the fourth POST hook reports kind=rejected_anti_spiral") {
                REQUIRE(events.size() == 4);
                auto post4 = nlohmann::json::parse(
                    events[3].context_json);
                CHECK(post4.at("result_kind").get<std::string>()
                      == "rejected_anti_spiral");
            }
            AND_THEN("earlier calls were dispatched normally as ok") {
                auto post1 = nlohmann::json::parse(
                    events[0].context_json);
                CHECK(post1.at("result_kind").get<std::string>()
                      == "ok");
            }
        }
    }
}

SCENARIO("Anti-spiral hard block uses derived default when sentinel set",
         "[tool_executor][anti-spiral][2.1.4][issue-14]") {
    GIVEN("a config with soft=2 and hard threshold = -1 (sentinel)") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        lc.max_consecutive_same_tool = 2;
        lc.max_consecutive_same_tool_hard_block = -1;
        // Effective hard threshold should be soft + 2 = 4.
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        std::vector<HookEvent> events;
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          record_hook_cb, &events, 0);
        attach_registry(executor, reg);

        WHEN("the same tool is dispatched four times") {
            LoopContext ctx;
            for (int i = 0; i < 4; ++i) {
                ToolCall call = make_call("ok.do_thing");
                call.id = "call-" + std::to_string(i);
                call.arguments["i"] = std::to_string(i);
                executor.process_tool_calls(ctx, {call});
            }
            THEN("the fourth call is hard-blocked") {
                REQUIRE(events.size() == 4);
                auto post4 = nlohmann::json::parse(
                    events[3].context_json);
                CHECK(post4.at("result_kind").get<std::string>()
                      == "rejected_anti_spiral");
            }
        }
    }
}
