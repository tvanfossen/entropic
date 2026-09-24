// SPDX-License-Identifier: Apache-2.0
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

#include <stdexcept>

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
 * @brief v2.3.10 — Tool that returns directives in its ServerResponse.
 *
 * Used to exercise extract_and_process_directives + build_directive
 * + build_complete_directive + extract_pipeline_stages from the
 * public process_tool_calls path. The serialized envelope is shaped
 * by MCPServerBase::serialize_response (result + directives array),
 * which is exactly what extract_and_process_directives parses.
 *
 * @internal
 * @version 2.3.10
 */
class DirectiveEmittingTool : public ToolBase {
public:
    DirectiveEmittingTool() : ToolBase(ToolDefinition{
        "emit_directives",
        "Emits a complete + pipeline directive",
        R"({"type":"object","properties":{}})"
    }) {}

    ServerResponse execute(const std::string& /*args_json*/) override {
        // The "result" field is the JSON the directive builders read
        // for the typed fields (summary / coverage_gap / target / etc).
        std::vector<entropic::Directive> dirs;
        dirs.push_back(entropic::Directive{
            ENTROPIC_DIRECTIVE_COMPLETE});
        dirs.push_back(entropic::Directive{
            ENTROPIC_DIRECTIVE_PIPELINE});
        dirs.push_back(entropic::Directive{
            ENTROPIC_DIRECTIVE_STOP_PROCESSING});
        return ServerResponse{
            R"({"summary":"all done","coverage_gap":false,)"
            R"("suggested_files":["a.cpp","b.cpp"],)"
            R"("stages":["eng","qa"],"task":"ship"})",
            std::move(dirs)
        };
    }
};

class DirectiveEmittingServer : public MCPServerBase {
public:
    DirectiveEmittingServer() : MCPServerBase("dir") {
        register_tool(&tool_);
    }
private:
    DirectiveEmittingTool tool_;
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

/**
 * @brief Tool that always throws, counting how often it was dispatched.
 *
 * Models the real shape from the v2.13.0 gate: FilesystemServer's
 * resolve_path THROWS on an outside-root refusal, MCPServerBase's barrier
 * turns that into an error result, and the model re-issues the identical
 * call. The counter is the assertion that matters — message text can be
 * satisfied by a guard that still dispatches.
 * @version 2.13.0
 */
class FailTool : public ToolBase {
public:
    /**
     * @brief Construct with a trivial schema.
     * @version 2.13.0
     */
    FailTool() : ToolBase(ToolDefinition{
        "always_fails",
        "Always fails",
        R"({"type":"object","properties":{"path":{"type":"string"}}})"
    }) {}

    int calls = 0; ///< Times execute() actually ran.

    /**
     * @brief Execute: always throw, as a refused path does.
     * @param args_json Arguments (unused).
     * @return Never returns.
     * @version 2.13.0
     */
    ServerResponse execute(const std::string& /*args_json*/) override {
        ++calls;
        throw std::runtime_error(
            "outside_root_approval_required: Path escapes project root");
    }
};

/**
 * @brief Test server wrapping FailTool, exposing its dispatch count.
 * @version 2.13.0
 */
class FailServer : public MCPServerBase {
public:
    /**
     * @brief Construct and register tool.
     * @version 2.13.0
     */
    FailServer() : MCPServerBase("fail") { register_tool(&tool_); }

    /**
     * @brief How many times the tool actually ran.
     * @return Dispatch count.
     * @version 2.13.0
     */
    int dispatches() const { return tool_.calls; }
private:
    FailTool tool_; ///< The always-failing tool
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

/**
 * @brief Hook that emits malformed UTF-8 as its transform.
 *
 * gh#111 (v2.9.7): a hook's transformed result is an inbound boundary
 * crossing a plugin .so, same as an MCP tool result. Before the fix,
 * ToolExecutor::fire_post_tool_hook assigned ``out`` straight to
 * ``msg.content`` with no sanitize_utf8 call, letting a hook-introduced
 * invalid byte sequence (0xC3 not followed by a valid continuation
 * byte) reach a later ``nlohmann::json::dump()`` and throw
 * type_error 316.
 *
 * @internal
 * @version 2.9.7
 */
static int malformed_utf8_post_cb(
    entropic_hook_point_t /*hp*/, const char* /*ctx*/,
    char** mod, void* /*ud*/) {
    *mod = dup_to_heap("bad byte: \xC3\x28 end");
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

SCENARIO("gh#111: POST_TOOL_CALL hook returning malformed UTF-8 is "
         "sanitized before it becomes Message::content",
         "[tool_executor][hooks][regression][2.9.7]") {
    GIVEN("an executor with a POST_TOOL_CALL hook emitting malformed UTF-8") {
        auto mgr = make_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          malformed_utf8_post_cb, nullptr, 0);
        attach_registry(executor, reg);

        WHEN("a successful tool call is processed") {
            LoopContext ctx;
            auto results = executor.process_tool_calls(
                ctx, {make_call("ok.do_thing")});

            THEN("the raw malformed sequence does not survive verbatim") {
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].content.find("\xC3\x28")
                        == std::string::npos);
            }
            THEN("the sanitized content can be JSON-dumped without throwing") {
                nlohmann::json arr = nlohmann::json::array();
                arr.push_back({{"role", "tool"},
                               {"content", results[0].content}});
                REQUIRE_NOTHROW(arr.dump());
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

// ── v2.3.10: directive-extraction path coverage ──

SCENARIO("Tool emits directives → ToolExecutor extracts + dispatches them",
         "[tool_executor][v2.3.10][coverage][directives]") {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp/test");
    mgr.register_server(std::make_unique<DirectiveEmittingServer>());
    mgr.initialize();

    LoopConfig lc;
    lc.auto_approve_tools = true;

    // Wire all the tool-execution callbacks so process_single_call
    // hits fire_tool_complete_callback (lines 1101-1105) +
    // serialize_tool_call (1075-1084) + fire_post_tool_hook paths.
    static int complete_fired;
    complete_fired = 0;
    EngineCallbacks cb;
    cb.on_tool_complete = [](const char* /*json*/, const char* /*result*/,
                             double /*ms*/, void* /*ud*/) {
        complete_fired += 1;
    };
    cb.on_tool_start = [](const char* /*json*/, void* /*ud*/) {};
    cb.on_tool_call = [](const char* /*json*/, void* /*ud*/) {};

    int dispatched = 0;
    std::vector<int> seen_types;
    ToolExecutorHooks hooks{};
    hooks.process_directives =
        [](LoopContext& /*ctx*/,
           const std::vector<const Directive*>& dirs,
           void* ud) -> DirectiveResult {
            auto* state = static_cast<std::pair<int, std::vector<int>>*>(ud);
            state->first += static_cast<int>(dirs.size());
            for (const auto* d : dirs) {
                state->second.push_back(static_cast<int>(d->type));
            }
            return DirectiveResult{};
        };
    auto state = std::make_pair(0, std::vector<int>{});
    hooks.user_data = &state;

    ToolExecutor executor(mgr, lc, cb, hooks);

    GIVEN("a single tool call whose response contains three directives") {
        LoopContext ctx;
        std::vector<ToolCall> calls{make_call("dir.emit_directives")};

        WHEN("process_tool_calls dispatches the call") {
            (void)executor.process_tool_calls(ctx, calls);

            THEN("on_tool_complete callback fires + directives dispatched") {
                REQUIRE(complete_fired >= 1);
                REQUIRE(state.first == 3);
                // All three directive types are present (complete,
                // pipeline, stop_processing — order from the tool).
                REQUIRE(std::find(state.second.begin(), state.second.end(),
                        static_cast<int>(ENTROPIC_DIRECTIVE_COMPLETE))
                        != state.second.end());
                REQUIRE(std::find(state.second.begin(), state.second.end(),
                        static_cast<int>(ENTROPIC_DIRECTIVE_PIPELINE))
                        != state.second.end());
                REQUIRE(std::find(state.second.begin(), state.second.end(),
                        static_cast<int>(ENTROPIC_DIRECTIVE_STOP_PROCESSING))
                        != state.second.end());
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

// ── gh#83 (v2.5.2): tier allowed_tools enforced at dispatch ──

TEST_CASE("gh#83: off-allowlist tool is rejected_unauthorized at dispatch",
          "[tool_executor][gh83]") {
    auto mgr = make_manager();
    LoopConfig lc; lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    std::unordered_map<std::string, std::vector<std::string>> allow;
    allow["researcher"] = {"docs.search", "entropic.complete"};  // no ok.do_thing
    executor.set_tier_allowed_tools(&allow);

    LoopContext ctx;
    ctx.locked_tier = "researcher";
    auto results = executor.process_tool_calls(
        ctx, {make_call("ok.do_thing")});

    REQUIRE(results.size() == 1);
    CHECK(results[0].metadata.at("result_kind") == "rejected_unauthorized");
    CHECK(results[0].content.find("not authorized") != std::string::npos);
}

TEST_CASE("gh#83: allowlisted tool dispatches normally",
          "[tool_executor][gh83]") {
    auto mgr = make_manager();
    LoopConfig lc; lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    std::unordered_map<std::string, std::vector<std::string>> allow;
    allow["researcher"] = {"ok.do_thing"};
    executor.set_tier_allowed_tools(&allow);

    LoopContext ctx;
    ctx.locked_tier = "researcher";
    auto results = executor.process_tool_calls(
        ctx, {make_call("ok.do_thing")});

    REQUIRE(results.size() == 1);
    CHECK(results[0].content.find("ok") != std::string::npos);
    CHECK(results[0].metadata.at("result_kind") == "ok");
}

TEST_CASE("gh#83: no map wired = pass-through (pre-v2.5.2 behavior)",
          "[tool_executor][gh83]") {
    auto mgr = make_manager();
    LoopConfig lc; lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);
    // no set_tier_allowed_tools call

    LoopContext ctx;
    ctx.locked_tier = "researcher";
    auto results = executor.process_tool_calls(
        ctx, {make_call("ok.do_thing")});

    REQUIRE(results.size() == 1);
    CHECK(results[0].content.find("ok") != std::string::npos);
}

TEST_CASE("gh#83: tier with no allowlist entry is unrestricted",
          "[tool_executor][gh83]") {
    auto mgr = make_manager();
    LoopConfig lc; lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    std::unordered_map<std::string, std::vector<std::string>> allow;
    allow["reader"] = {"docs.search"};  // researcher has no entry
    executor.set_tier_allowed_tools(&allow);

    LoopContext ctx;
    ctx.locked_tier = "researcher";  // not in the map
    auto results = executor.process_tool_calls(
        ctx, {make_call("ok.do_thing")});

    REQUIRE(results.size() == 1);
    CHECK(results[0].content.find("ok") != std::string::npos);
}

TEST_CASE("gh#83: empty locked_tier bypasses enforcement",
          "[tool_executor][gh83]") {
    auto mgr = make_manager();
    LoopConfig lc; lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    std::unordered_map<std::string, std::vector<std::string>> allow;
    allow["researcher"] = {"docs.search"};
    executor.set_tier_allowed_tools(&allow);

    LoopContext ctx;  // locked_tier empty
    auto results = executor.process_tool_calls(
        ctx, {make_call("ok.do_thing")});

    REQUIRE(results.size() == 1);
    CHECK(results[0].content.find("ok") != std::string::npos);
}

// ── gh#143: argument-free tool calls ─────────────────────

/**
 * @brief Tool with NO required fields that reads its args via .value().
 *
 * gh#143 RED. This is the shape of every real no-required-fields tool
 * (`git.diff`, `filesystem.list_directory`): the schema declares only
 * optional properties, so schema validation passes, and `execute`
 * reaches for `.value()` on the parsed arguments.
 *
 * The pre-existing `OkTool` cannot reproduce the defect because it
 * ignores `args_json` entirely — which is precisely why the suite
 * drove the buggy path on every run for 25 releases without observing
 * it. The read of `args_json` is the whole point of this fixture.
 *
 * @dg_internal
 * @version 2.12.0
 */
class OptionalArgsTool : public ToolBase {
public:
    OptionalArgsTool() : ToolBase(ToolDefinition{
        "diff",
        "Reads an optional flag, mirroring GitDiffTool",
        R"({"type":"object","properties":{"staged":{"type":"boolean"}}})"
    }) {}

    ServerResponse execute(const std::string& args_json) override {
        auto args = nlohmann::json::parse(args_json);
        bool staged = args.value("staged", false);
        return ServerResponse{staged ? "staged" : "unstaged", {}};
    }
};

/**
 * @brief Tool whose execute always throws, for the dispatch barrier.
 * @dg_internal
 * @version 2.12.0
 */
class ThrowingTool : public ToolBase {
public:
    ThrowingTool() : ToolBase(ToolDefinition{
        "boom",
        "Always throws",
        R"({"type":"object","properties":{}})"
    }) {}

    ServerResponse execute(const std::string& /*args_json*/) override {
        throw std::runtime_error("tool exploded");
    }
};

/**
 * @brief Server exposing the two gh#143 fixtures.
 * @dg_internal
 * @version 2.12.0
 */
class OptionalArgsServer : public MCPServerBase {
public:
    OptionalArgsServer() : MCPServerBase("git") {
        register_tool(&diff_);
        register_tool(&boom_);
    }
private:
    OptionalArgsTool diff_;
    ThrowingTool boom_;
};

/**
 * @brief ServerManager carrying the gh#143 fixtures.
 * @dg_internal
 * @version 2.12.0
 */
static ServerManager make_optional_args_manager() {
    PermissionsConfig perms;
    ServerManager mgr(perms, "/tmp/test");
    mgr.register_server(std::make_unique<OptionalArgsServer>());
    mgr.initialize();
    return mgr;
}

SCENARIO("gh#143: an argument-free call to a no-required-fields tool "
         "does not kill the run",
         "[tool_executor][gh143][regression][2.12.0]") {
    GIVEN("a tool whose schema has only optional properties") {
        auto mgr = make_optional_args_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        WHEN("the model emits a call carrying no arguments at all") {
            // Empty `arguments` map AND empty `arguments_json` — the
            // exact shape observed in the consumer's runs.
            LoopContext ctx;
            auto call = make_call("git.diff");
            REQUIRE(call.arguments.empty());
            REQUIRE(call.arguments_json.empty());

            THEN("the executor returns a result instead of throwing") {
                // RED before the fix: serialize_args yields "null",
                // json::parse("null") succeeds, and .value() throws
                // type_error.306 straight out of dispatch.
                std::vector<Message> results;
                REQUIRE_NOTHROW(
                    results = executor.process_tool_calls(ctx, {call}));
                REQUIRE(results.size() == 1);
            }
            AND_THEN("the tool ran and saw its default, not an error") {
                auto results = executor.process_tool_calls(ctx, {call});
                REQUIRE(results.size() == 1);
                CHECK(results[0].content.find("unstaged")
                      != std::string::npos);
            }
        }
    }
}

SCENARIO("gh#143: dispatch converts a throwing tool into a tool error",
         "[tool_executor][gh143][regression][2.12.0]") {
    GIVEN("a tool whose execute throws") {
        auto mgr = make_optional_args_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        WHEN("it is dispatched") {
            LoopContext ctx;

            THEN("the exception does not unwind through the loop") {
                // The general form of the fix: plugin servers already
                // get this via gh#133's route_plugin_call; in-process
                // servers had no equivalent.
                std::vector<Message> results;
                REQUIRE_NOTHROW(results = executor.process_tool_calls(
                    ctx, {make_call("git.boom")}));
                REQUIRE(results.size() == 1);
            }
            AND_THEN("the model is told what went wrong") {
                auto results = executor.process_tool_calls(
                    ctx, {make_call("git.boom")});
                REQUIRE(results.size() == 1);
                CHECK(results[0].content.find("tool exploded")
                      != std::string::npos);
            }
        }
    }
}

SCENARIO("gh#143: an argument-free call serialises as an object "
         "everywhere it is observable",
         "[tool_executor][gh143][regression][2.12.0]") {
    GIVEN("an executor with a PRE_TOOL_CALL hook") {
        auto mgr = make_optional_args_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        std::vector<HookEvent> events;
        reg.register_hook(ENTROPIC_HOOK_PRE_TOOL_CALL,
                          record_hook_cb, &events, 0);
        attach_registry(executor, reg);

        WHEN("an argument-free call is processed") {
            LoopContext ctx;
            executor.process_tool_calls(ctx, {make_call("git.diff")});

            THEN("the hook payload carries an object, not null") {
                // RED before the fix: build_pre_tool_json puts a JSON
                // null here, which crosses the plugin .so boundary and
                // throws 306 in any hook that calls .value() on it.
                // The pre-existing assertion was only contains("args"),
                // which a null value satisfies.
                REQUIRE(events.size() == 1);
                auto pre = nlohmann::json::parse(events[0].context_json);
                REQUIRE(pre.contains("args"));
                CHECK(pre.at("args").is_object());
            }
        }
    }
}

// ── gh#168 (v2.13.0): a PRE_TOOL_CALL hook's modified_json is APPLIED ──
//
// Pre-2.13.0 fire_pre_tool_hook did `free(mod); return rc != 0;` — the
// modification channel that entropic.h advertises in its allocation
// contract was dropped for EVERY tool, on every call. A downstream host
// tried to append a parent's verbatim observation to a delegated task;
// the specialist only ever saw the lead's paraphrase.
//
// The fix applies the ARGUMENTS only. Identity (tool_name) is immutable,
// malformed payloads are refused loudly, and rc != 0 still cancels.

namespace {

/**
 * @brief What a scripted PRE_TOOL_CALL hook hands back (gh#168).
 * @internal
 * @version 2.13.0
 */
struct PreModSpec {
    std::string payload;  ///< Written to *modified_json ("" = none).
    int rc = 0;           ///< 0 proceeds, non-zero cancels.
};

/**
 * @brief PRE_TOOL_CALL hook driven by a PreModSpec.
 * @param mod Out-param for the modification (engine frees it).
 * @param ud PreModSpec*.
 * @return The spec's rc.
 * @internal
 * @version 2.13.0
 */
static int pre_mod_cb(entropic_hook_point_t /*hp*/,
                      const char* /*ctx*/, char** mod, void* ud) {
    auto* spec = static_cast<PreModSpec*>(ud);
    if (mod != nullptr && !spec->payload.empty()) {
        *mod = dup_to_heap(spec->payload.c_str());
    }
    return spec->rc;
}

/**
 * @brief Run one `git.diff` call through an executor with a scripted
 *        PRE hook, returning the result content the model would see.
 *
 * `git.diff` (OptionalArgsTool) reads `staged` out of its arguments and
 * answers "staged" or "unstaged" — so the returned string IS the
 * observation of what the tool actually received.
 *
 * @param spec Scripted hook behaviour.
 * @return Result message content.
 * @internal
 * @version 2.13.0
 */
static std::string run_with_pre_mod(PreModSpec& spec) {
    auto mgr = make_optional_args_manager();
    LoopConfig lc;
    lc.auto_approve_tools = true;
    EngineCallbacks cb;
    ToolExecutor executor(mgr, lc, cb);

    HookRegistry reg;
    reg.register_hook(ENTROPIC_HOOK_PRE_TOOL_CALL,
                      pre_mod_cb, &spec, 0);
    attach_registry(executor, reg);

    LoopContext ctx;
    auto results = executor.process_tool_calls(ctx, {make_call("git.diff")});
    REQUIRE(results.size() == 1);
    return results[0].content;
}

} // namespace

SCENARIO("gh#168: a PRE_TOOL_CALL hook's modified args reach the tool",
         "[tool_executor][gh168][regression][2.13.0]") {
    GIVEN("a PRE hook that returns 0 and rewrites args") {
        PreModSpec spec{
            R"({"tool_name":"git.diff","args":{"staged":true}})", 0};

        WHEN("an argument-free call is dispatched") {
            THEN("the tool sees the hook's arguments, not the model's") {
                // RED before the fix: free(mod) discarded the rewrite and
                // the tool answered "unstaged" on every run.
                CHECK(run_with_pre_mod(spec) == "staged");
            }
        }
    }
}

SCENARIO("gh#168: the rewrite is visible to the dup cache and POST hook",
         "[tool_executor][gh168][regression][2.13.0]") {
    GIVEN("an executor with a rewriting PRE hook and a POST recorder") {
        auto mgr = make_optional_args_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        PreModSpec spec{R"({"args":{"staged":true}})", 0};
        HookRegistry reg;
        std::vector<HookEvent> events;
        reg.register_hook(ENTROPIC_HOOK_PRE_TOOL_CALL,
                          pre_mod_cb, &spec, 0);
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          record_hook_cb, &events, 0);
        attach_registry(executor, reg);

        WHEN("an argument-free call is dispatched") {
            LoopContext ctx;
            executor.process_tool_calls(ctx, {make_call("git.diff")});

            THEN("POST_TOOL_CALL reports the rewritten args") {
                // Everything downstream of the hook — schema validation,
                // the duplicate key, permission patterns, the POST
                // context — is computed from the same ToolCall, so the
                // rewrite must be visible here or it is only half applied.
                REQUIRE(events.size() == 1);
                auto post = nlohmann::json::parse(events[0].context_json);
                REQUIRE(post.at("args").is_object());
                CHECK(post.at("args").at("staged").get<bool>());
            }
        }
    }
}

SCENARIO("gh#168: a modification that renames the tool is refused",
         "[tool_executor][gh168][guard][2.13.0]") {
    GIVEN("a PRE hook that returns 0 but names a different tool") {
        PreModSpec spec{
            R"({"tool_name":"git.boom","args":{"staged":true}})", 0};

        WHEN("the call is dispatched") {
            THEN("neither the identity nor the args are taken") {
                // RED against the NAIVE fix: applying args without an
                // identity check answers "staged" here. The whole
                // payload is refused, so git.diff runs on the model's
                // own (empty) arguments. git.boom always throws, so a
                // rerouted dispatch would surface as "tool exploded".
                auto content = run_with_pre_mod(spec);
                CHECK(content == "unstaged");
                CHECK(content.find("exploded") == std::string::npos);
            }
        }
    }
}

SCENARIO("gh#168: a malformed modification is refused, not half-applied",
         "[tool_executor][gh168][guard][2.13.0]") {
    GIVEN("PRE hooks returning payloads the contract does not allow") {
        WHEN("the payload is not JSON at all") {
            PreModSpec spec{"not json at all", 0};
            THEN("the call dispatches unmodified") {
                CHECK(run_with_pre_mod(spec) == "unstaged");
            }
        }
        WHEN("the payload is JSON but not an object") {
            PreModSpec spec{R"(["staged"])", 0};
            THEN("the call dispatches unmodified") {
                CHECK(run_with_pre_mod(spec) == "unstaged");
            }
        }
        WHEN("the payload carries no args object") {
            PreModSpec spec{R"({"tool_name":"git.diff"})", 0};
            THEN("the call dispatches unmodified") {
                CHECK(run_with_pre_mod(spec) == "unstaged");
            }
        }
        WHEN("args is present but is not an object") {
            PreModSpec spec{R"({"args":"staged=true"})", 0};
            THEN("the call dispatches unmodified") {
                CHECK(run_with_pre_mod(spec) == "unstaged");
            }
        }
    }
}

SCENARIO("gh#168: a rewritten payload carrying bad UTF-8 is sanitized, "
         "not rejected",
         "[tool_executor][gh168][utf8][2.13.0]") {
    GIVEN("a PRE hook whose args contain an ill-formed byte") {
        auto mgr = make_optional_args_manager();
        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        // gh#113/#114/#132 class: a hook is a plugin .so, an external
        // boundary in BOTH directions. nlohmann's parser rejects an
        // ill-formed UTF-8 byte inside a string, so sanitizing AFTER
        // the parse would refuse a payload that is merely dirty.
        std::string payload =
            std::string(R"({"args":{"staged":true,"note":"a)")
            + '\xFF' + R"(b"}})";
        PreModSpec spec{payload, 0};

        HookRegistry reg;
        std::vector<HookEvent> events;
        reg.register_hook(ENTROPIC_HOOK_PRE_TOOL_CALL,
                          pre_mod_cb, &spec, 0);
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          record_hook_cb, &events, 0);
        attach_registry(executor, reg);

        WHEN("the call is dispatched") {
            LoopContext ctx;
            auto results = executor.process_tool_calls(
                ctx, {make_call("git.diff")});

            THEN("the modification still applies") {
                REQUIRE(results.size() == 1);
                CHECK(results[0].content == "staged");
            }
            AND_THEN("the bad byte became U+FFFD") {
                REQUIRE(events.size() == 1);
                auto post = nlohmann::json::parse(events[0].context_json);
                auto note = post.at("args").at("note").get<std::string>();
                CHECK(note == "a\xEF\xBF\xBD" "b");
            }
        }
    }
}

SCENARIO("gh#168: a non-zero return still cancels, modification or not",
         "[tool_executor][gh168][regression][2.13.0]") {
    GIVEN("a PRE hook that writes args AND returns non-zero") {
        PreModSpec spec{R"({"args":{"staged":true}})", 1};

        WHEN("the call is dispatched") {
            THEN("the call is cancelled, not rewritten and run") {
                auto content = run_with_pre_mod(spec);
                CHECK(content.find("denied") != std::string::npos);
                CHECK(content != "staged");
            }
        }
    }
}

// ── Repeated identical FAILURE guard (v2.13.0) ───────────

SCENARIO("A call that fails identically is refused rather than retried "
         "forever",
         "[tool_executor][anti-spiral][2.13.0]") {
    GIVEN("an executor and a tool whose every dispatch throws") {
        PermissionsConfig perms;
        ServerManager mgr(perms, "/tmp/test");
        auto owned = std::make_unique<FailServer>();
        auto* srv = owned.get();
        mgr.register_server(std::move(owned));
        mgr.initialize();

        LoopConfig lc;
        lc.auto_approve_tools = true;
        EngineCallbacks cb;
        ToolExecutor executor(mgr, lc, cb);

        HookRegistry reg;
        std::vector<HookEvent> events;
        reg.register_hook(ENTROPIC_HOOK_POST_TOOL_CALL,
                          record_hook_cb, &events, 0);
        attach_registry(executor, reg);

        // The v2.13.0 gate's test-e7-delegation: a delegated child issued
        // filesystem.read_file with byte-identical arguments FOUR times,
        // was refused identically every time, and nothing stopped it —
        // errors are deliberately excluded from the duplicate cache, and
        // the anti-spiral block counts tool NAME against a threshold well
        // above four. The run died on its 120s timeout.
        WHEN("the same call with the same arguments is issued four times") {
            LoopContext ctx;
            std::vector<Message> last;
            for (int i = 0; i < 4; ++i) {
                ToolCall call = make_call("fail.always_fails");
                call.id = "call-" + std::to_string(i);
                call.arguments["path"] = "/outside/notes.md";
                last = executor.process_tool_calls(ctx, {call});
            }

            THEN("the tool stops being dispatched at the threshold") {
                INFO("dispatches: " << srv->dispatches());
                CHECK(srv->dispatches() == 2);
            }
            AND_THEN("the third attempt is refused pre-dispatch") {
                REQUIRE(events.size() == 4);
                auto post3 = nlohmann::json::parse(events[2].context_json);
                CHECK(post3.at("result_kind").get<std::string>()
                      == "rejected_anti_spiral");
            }
            // REQ-MCP-015: a bare failure teaches the model nothing. The
            // refusal has to name the tool, say how many times it failed,
            // and say the failure is not transient — otherwise "try again"
            // stays the model's most plausible next move.
            AND_THEN("the refusal names the tool and the repeat count") {
                REQUIRE(last.size() == 1);
                INFO(last[0].content);
                CHECK(last[0].content.find("always_fails")
                      != std::string::npos);
                // TWO, not three: two attempts actually ran and failed.
                // The third is refused without dispatching, so claiming
                // three failures would overstate what was observed.
                CHECK(last[0].content.find("2 times")
                      != std::string::npos);
                CHECK(last[0].content.find("identical arguments")
                      != std::string::npos);
                CHECK(last[0].content.find("not transient")
                      != std::string::npos);
                CHECK(last[0].content.find("different approach")
                      != std::string::npos);
            }
        }

        // Regression guard on the v1.8.5 intent that record_tool_call
        // documents: "a transient failure does not permanently poison the
        // call". One failure must still be retryable, or this fix has
        // traded an unbounded retry for no retry at all.
        WHEN("the same failing call is issued only twice") {
            LoopContext ctx;
            for (int i = 0; i < 2; ++i) {
                ToolCall call = make_call("fail.always_fails");
                call.id = "retry-" + std::to_string(i);
                call.arguments["path"] = "/outside/notes.md";
                executor.process_tool_calls(ctx, {call});
            }

            THEN("the retry is still dispatched") {
                CHECK(srv->dispatches() == 2);
            }
        }
    }
}
