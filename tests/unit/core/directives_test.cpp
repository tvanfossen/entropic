// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_directives.cpp
 * @brief DirectiveProcessor unit tests.
 * @version 1.8.4
 */

#include <entropic/core/directives.h>
#include <entropic/core/engine_types.h>
#include <catch2/catch_test_macros.hpp>

using namespace entropic;

// ── Helper ───────────────────────────────────────────────

/**
 * @brief Create a default LoopContext for testing.
 * @return Initialized LoopContext.
 * @internal
 * @version 1.8.4
 */
static LoopContext make_ctx() {
    LoopContext ctx;
    ctx.state = AgentState::EXECUTING;
    return ctx;
}

// ── DirectiveProcessor tests ─────────────────────────────

TEST_CASE("StopProcessing sets flag", "[directives]") {
    DirectiveProcessor dp;
    bool called = false;
    dp.register_handler(ENTROPIC_DIRECTIVE_STOP_PROCESSING,
        [&](LoopContext&, const Directive&, DirectiveResult& r) {
            r.stop_processing = true;
            called = true;
        });
    auto ctx = make_ctx();
    StopProcessingDirective d;
    std::vector<const Directive*> dirs = {&d};
    auto result = dp.process(ctx, dirs);
    REQUIRE(result.stop_processing);
    REQUIRE(called);
}

TEST_CASE("TierChange updates locked_tier", "[directives]") {
    DirectiveProcessor dp;
    dp.register_handler(ENTROPIC_DIRECTIVE_TIER_CHANGE,
        [](LoopContext& ctx, const Directive& d, DirectiveResult& r) {
            const auto& tc = static_cast<const TierChangeDirective&>(d);
            ctx.locked_tier = tc.tier;
            r.tier_changed = true;
        });
    auto ctx = make_ctx();
    TierChangeDirective d("eng", "test");
    std::vector<const Directive*> dirs = {&d};
    auto result = dp.process(ctx, dirs);
    REQUIRE(ctx.locked_tier == "eng");
    REQUIRE(result.tier_changed);
}

TEST_CASE("Delegate stores pending", "[directives]") {
    DirectiveProcessor dp;
    dp.register_handler(ENTROPIC_DIRECTIVE_DELEGATE,
        [](LoopContext& ctx, const Directive& d, DirectiveResult& r) {
            const auto& dl = static_cast<const DelegateDirective&>(d);
            ctx.metadata["pending_delegation"] = dl.target;
            r.stop_processing = true;
        });
    auto ctx = make_ctx();
    DelegateDirective d("eng", "write tests", 5);
    std::vector<const Directive*> dirs = {&d};
    dp.process(ctx, dirs);
    REQUIRE(ctx.metadata["pending_delegation"] == "eng");
}

TEST_CASE("Pipeline stores pending", "[directives]") {
    DirectiveProcessor dp;
    dp.register_handler(ENTROPIC_DIRECTIVE_PIPELINE,
        [](LoopContext& ctx, const Directive& d, DirectiveResult& r) {
            const auto& pl = static_cast<const PipelineDirective&>(d);
            ctx.metadata["pending_pipeline"] = pl.task;
            r.stop_processing = true;
        });
    auto ctx = make_ctx();
    PipelineDirective d({"lead", "eng"}, "build thing");
    std::vector<const Directive*> dirs = {&d};
    dp.process(ctx, dirs);
    REQUIRE(ctx.metadata["pending_pipeline"] == "build thing");
}

TEST_CASE("Complete sets COMPLETE state", "[directives]") {
    DirectiveProcessor dp;
    dp.register_handler(ENTROPIC_DIRECTIVE_COMPLETE,
        [](LoopContext& ctx, const Directive& d, DirectiveResult& r) {
            const auto& cd = static_cast<const CompleteDirective&>(d);
            ctx.metadata["summary"] = cd.summary;
            ctx.state = AgentState::COMPLETE;
            r.stop_processing = true;
        });
    auto ctx = make_ctx();
    CompleteDirective d("all done");
    std::vector<const Directive*> dirs = {&d};
    dp.process(ctx, dirs);
    REQUIRE(ctx.state == AgentState::COMPLETE);
    REQUIRE(ctx.metadata["summary"] == "all done");
}

TEST_CASE("InjectContext adds to injected_messages",
          "[directives]") {
    DirectiveProcessor dp;
    dp.register_handler(ENTROPIC_DIRECTIVE_INJECT_CONTEXT,
        [](LoopContext&, const Directive& d, DirectiveResult& r) {
            const auto& ic = static_cast<const InjectContextDirective&>(d);
            Message m;
            m.role = ic.role;
            m.content = ic.content;
            r.injected_messages.push_back(std::move(m));
        });
    auto ctx = make_ctx();
    InjectContextDirective d("extra context", "user");
    std::vector<const Directive*> dirs = {&d};
    auto result = dp.process(ctx, dirs);
    REQUIRE(result.injected_messages.size() == 1);
    REQUIRE(result.injected_messages[0].content == "extra context");
}

TEST_CASE("PhaseChange updates active_phase",
          "[directives]") {
    DirectiveProcessor dp;
    dp.register_handler(ENTROPIC_DIRECTIVE_PHASE_CHANGE,
        [](LoopContext& ctx, const Directive& d, DirectiveResult&) {
            const auto& pc = static_cast<const PhaseChangeDirective&>(d);
            ctx.active_phase = pc.phase;
        });
    auto ctx = make_ctx();
    PhaseChangeDirective d("analysis");
    std::vector<const Directive*> dirs = {&d};
    dp.process(ctx, dirs);
    REQUIRE(ctx.active_phase == "analysis");
}

TEST_CASE("NotifyPresenter fires callback",
          "[directives]") {
    bool called = false;
    DirectiveProcessor dp;
    dp.register_handler(ENTROPIC_DIRECTIVE_NOTIFY_PRESENTER,
        [&](LoopContext&, const Directive&, DirectiveResult&) {
            called = true;
        });
    auto ctx = make_ctx();
    NotifyPresenterDirective d("key", "{}");
    std::vector<const Directive*> dirs = {&d};
    dp.process(ctx, dirs);
    REQUIRE(called);
}

TEST_CASE("Unknown directive type logs warning",
          "[directives]") {
    DirectiveProcessor dp;
    // Register nothing
    auto ctx = make_ctx();
    StopProcessingDirective d;
    std::vector<const Directive*> dirs = {&d};
    // Should not crash, just skip
    auto result = dp.process(ctx, dirs);
    REQUIRE_FALSE(result.stop_processing);
}

TEST_CASE("StopProcessing halts further processing",
          "[directives]") {
    DirectiveProcessor dp;
    int call_count = 0;
    auto handler = [&](LoopContext&, const Directive&,
                       DirectiveResult& r) {
        call_count++;
        r.stop_processing = true;
    };
    dp.register_handler(ENTROPIC_DIRECTIVE_STOP_PROCESSING, handler);
    dp.register_handler(ENTROPIC_DIRECTIVE_TIER_CHANGE, handler);

    auto ctx = make_ctx();
    StopProcessingDirective d1;
    TierChangeDirective d2("eng", "test");
    std::vector<const Directive*> dirs = {&d1, &d2};
    dp.process(ctx, dirs);
    REQUIRE(call_count == 1); // Second handler never called
}

TEST_CASE("All 11 directive types construct correctly",
          "[directives]") {
    StopProcessingDirective d1;
    REQUIRE(d1.type == ENTROPIC_DIRECTIVE_STOP_PROCESSING);
    TierChangeDirective d2("t", "r");
    REQUIRE(d2.type == ENTROPIC_DIRECTIVE_TIER_CHANGE);
    DelegateDirective d3("t", "task");
    REQUIRE(d3.type == ENTROPIC_DIRECTIVE_DELEGATE);
    PipelineDirective d4({"a"}, "t");
    REQUIRE(d4.type == ENTROPIC_DIRECTIVE_PIPELINE);
    CompleteDirective d5("s");
    REQUIRE(d5.type == ENTROPIC_DIRECTIVE_COMPLETE);
    ClearSelfTodosDirective d6;
    REQUIRE(d6.type == ENTROPIC_DIRECTIVE_CLEAR_SELF_TODOS);
    InjectContextDirective d7("c");
    REQUIRE(d7.type == ENTROPIC_DIRECTIVE_INJECT_CONTEXT);
    PruneMessagesDirective d8(3);
    REQUIRE(d8.type == ENTROPIC_DIRECTIVE_PRUNE_MESSAGES);
    ContextAnchorDirective d9("k", "v");
    REQUIRE(d9.type == ENTROPIC_DIRECTIVE_CONTEXT_ANCHOR);
    PhaseChangeDirective d10("p");
    REQUIRE(d10.type == ENTROPIC_DIRECTIVE_PHASE_CHANGE);
    NotifyPresenterDirective d11("k", "d");
    REQUIRE(d11.type == ENTROPIC_DIRECTIVE_NOTIFY_PRESENTER);
}

// ── v2.3.10: backstop coverage for the hook-dispatch path ──────────

namespace {
struct HookCallLog {
    int pre_calls = 0;
    entropic_hook_point_t last_point = ENTROPIC_HOOK_PRE_GENERATE;
    int next_return = 0;
};
static int stub_fire_pre(void* registry, entropic_hook_point_t point,
                         const char*, char** out_json) {
    auto* log = static_cast<HookCallLog*>(registry);
    log->pre_calls++;
    log->last_point = point;
    *out_json = nullptr;
    return log->next_return;
}
} // namespace

TEST_CASE("DirectiveProcessor hook-dispatch paths "
          "[v2.3.10][core][directives_coverage]", "[directives]") {
    SECTION("ON_DIRECTIVE for known type, handler runs") {
        DirectiveProcessor dp; HookCallLog log;
        HookInterface hi{}; hi.fire_pre = stub_fire_pre; hi.registry = &log;
        dp.set_hooks(hi);
        int handler_calls = 0;
        dp.register_handler(ENTROPIC_DIRECTIVE_TIER_CHANGE,
            [&](LoopContext&, const Directive&, DirectiveResult&) {
                handler_calls++;
            });
        auto ctx = make_ctx();
        TierChangeDirective d("eng", "r");
        std::vector<const Directive*> dirs = {&d};
        dp.process(ctx, dirs);
        REQUIRE(log.pre_calls == 1);
        REQUIRE(log.last_point == ENTROPIC_HOOK_ON_DIRECTIVE);
        REQUIRE(handler_calls == 1);
    }
    SECTION("ON_CUSTOM_DIRECTIVE when no handler is registered") {
        DirectiveProcessor dp; HookCallLog log;
        HookInterface hi{}; hi.fire_pre = stub_fire_pre; hi.registry = &log;
        dp.set_hooks(hi);
        auto ctx = make_ctx();
        StopProcessingDirective d;
        std::vector<const Directive*> dirs = {&d};
        dp.process(ctx, dirs);
        REQUIRE(log.pre_calls == 1);
        REQUIRE(log.last_point == ENTROPIC_HOOK_ON_CUSTOM_DIRECTIVE);
    }
    SECTION("hook returns non-zero -> handler suppressed") {
        DirectiveProcessor dp;
        HookCallLog log; log.next_return = 1;
        HookInterface hi{}; hi.fire_pre = stub_fire_pre; hi.registry = &log;
        dp.set_hooks(hi);
        int handler_calls = 0;
        dp.register_handler(ENTROPIC_DIRECTIVE_PHASE_CHANGE,
            [&](LoopContext&, const Directive&, DirectiveResult&) {
                handler_calls++;
            });
        auto ctx = make_ctx();
        PhaseChangeDirective d("analysis");
        std::vector<const Directive*> dirs = {&d};
        dp.process(ctx, dirs);
        REQUIRE(log.pre_calls == 1);
        REQUIRE(handler_calls == 0);
    }
    SECTION("null directive pointers in batch are skipped") {
        DirectiveProcessor dp;
        int handler_calls = 0;
        dp.register_handler(ENTROPIC_DIRECTIVE_NOTIFY_PRESENTER,
            [&](LoopContext&, const Directive&, DirectiveResult&) {
                handler_calls++;
            });
        auto ctx = make_ctx();
        NotifyPresenterDirective d("k", "{}");
        std::vector<const Directive*> dirs = {nullptr, &d, nullptr};
        dp.process(ctx, dirs);
        REQUIRE(handler_calls == 1);
    }
    SECTION("empty list yields default-empty result") {
        DirectiveProcessor dp;
        auto ctx = make_ctx();
        std::vector<const Directive*> dirs;
        auto r = dp.process(ctx, dirs);
        REQUIRE_FALSE(r.stop_processing);
        REQUIRE_FALSE(r.tier_changed);
        REQUIRE(r.injected_messages.empty());
    }
    SECTION("directive constructor defaults + overrides") {
        TierChangeDirective t;       REQUIRE(t.tier.empty());
        DelegateDirective dd;        REQUIRE(dd.max_turns == -1);
        DelegateDirective dd2("e","t",7,"id");
        REQUIRE(dd2.resume_from_delegation_id == "id");
        PipelineDirective p;         REQUIRE(p.stages.empty());
        CompleteDirective c;         REQUIRE_FALSE(c.coverage_gap);
        InjectContextDirective ic;   REQUIRE(ic.role == "user");
        PruneMessagesDirective pm;   REQUIRE(pm.keep_recent == 2);
        ContextAnchorDirective ca;   REQUIRE(ca.key.empty());
        PhaseChangeDirective pc;     REQUIRE(pc.phase.empty());
        NotifyPresenterDirective np; REQUIRE(np.key.empty());
    }
}
