// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_delegation_manager.cpp
 * @brief DelegationManager unit tests.
 * @version 1.8.6
 */

#include <entropic/core/delegation.h>
#include <entropic/core/engine_types.h>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace entropic;

// ── Mock tier resolution ─────────────────────────────────

/**
 * @brief Mock tier resolution state.
 * @internal
 * @version 1.8.6
 */
struct MockTierResolution {
    bool tier_valid = true;
    std::string system_prompt = "You are an engineer.";
    bool explicit_completion = true;
    std::string completion_instructions = "Use entropic.complete when done.";
};

/**
 * @brief Mock resolve_tier callback.
 * @internal
 * @version 1.8.6
 */
static ChildContextInfo mock_resolve_tier(
    const std::string& tier_name, void* ud) {
    auto* mock = static_cast<MockTierResolution*>(ud);
    ChildContextInfo info;
    info.valid = mock->tier_valid;
    info.system_prompt = mock->system_prompt;
    info.explicit_completion = mock->explicit_completion;
    info.completion_instructions = mock->completion_instructions;
    (void)tier_name;
    return info;
}

/**
 * @brief Mock tier_exists callback.
 * @internal
 * @version 1.8.6
 */
static bool mock_tier_exists(
    const std::string& /*tier_name*/, void* ud) {
    auto* mock = static_cast<MockTierResolution*>(ud);
    return mock->tier_valid;
}

/**
 * @brief Build a mock TierResolutionInterface.
 * @param mock Mock state.
 * @return Wired interface.
 * @internal
 * @version 1.8.6
 */
static TierResolutionInterface make_mock_tier_res(
    MockTierResolution& mock) {
    TierResolutionInterface res;
    res.resolve_tier = mock_resolve_tier;
    res.tier_exists = mock_tier_exists;
    res.user_data = &mock;
    return res;
}

// ── Mock child loop ──────────────────────────────────────

/**
 * @brief Mock child loop state — tracks calls and sets result.
 * @internal
 * @version 1.8.6
 */
struct MockChildLoop {
    int call_count = 0;
    bool complete = true;
    std::string response = "Task completed successfully.";
};

/**
 * @brief Mock run_child_loop callback.
 * @internal
 * @version 1.8.6
 */
static void mock_run_child(LoopContext& ctx, void* ud) {
    auto* mock = static_cast<MockChildLoop*>(ud);
    mock->call_count++;

    Message assistant;
    assistant.role = "assistant";
    assistant.content = mock->response;
    ctx.messages.push_back(std::move(assistant));

    if (mock->complete) {
        ctx.state = AgentState::COMPLETE;
    }
    ctx.metrics.iterations = 3;
}

// ── Tests ────────────────────────────────────────────────

TEST_CASE("execute_delegation returns error for unknown tier",
          "[delegation]") {
    MockTierResolution tier_mock;
    tier_mock.tier_valid = false;
    auto tier_res = make_mock_tier_res(tier_mock);

    MockChildLoop loop_mock;

    DelegationManager mgr(mock_run_child, &loop_mock, tier_res);
    LoopContext parent;

    auto result = mgr.execute_delegation(parent, "unknown", "do stuff");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.summary.find("Unknown tier") != std::string::npos);
    REQUIRE(loop_mock.call_count == 0);
}

TEST_CASE("child context has correct depth",
          "[delegation]") {
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    int child_depth = -1;
    auto capture_loop = [](LoopContext& ctx, void* ud) {
        auto* depth = static_cast<int*>(ud);
        *depth = ctx.delegation_depth;
        ctx.state = AgentState::COMPLETE;
        Message m;
        m.role = "assistant";
        m.content = "done";
        ctx.messages.push_back(std::move(m));
    };

    DelegationManager mgr(capture_loop, &child_depth, tier_res);

    LoopContext parent;
    parent.delegation_depth = 0;

    mgr.execute_delegation(parent, "eng", "build it");
    REQUIRE(child_depth == 1);
}

TEST_CASE("child context has fresh messages",
          "[delegation]") {
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    int child_msg_count = -1;
    auto capture_loop = [](LoopContext& ctx, void* ud) {
        auto* count = static_cast<int*>(ud);
        *count = static_cast<int>(ctx.messages.size());
        ctx.state = AgentState::COMPLETE;
        Message m;
        m.role = "assistant";
        m.content = "done";
        ctx.messages.push_back(std::move(m));
    };

    DelegationManager mgr(capture_loop, &child_msg_count, tier_res);

    LoopContext parent;
    // Add some parent history
    Message pm;
    pm.role = "user";
    pm.content = "parent message";
    parent.messages.push_back(pm);
    parent.messages.push_back(pm);

    mgr.execute_delegation(parent, "eng", "build it");
    // Child should have only system + user (2 messages)
    REQUIRE(child_msg_count == 2);
}

TEST_CASE("completion instructions injected for explicit_completion",
          "[delegation]") {
    MockTierResolution tier_mock;
    tier_mock.explicit_completion = true;
    tier_mock.completion_instructions = "Use entropic.complete.";
    auto tier_res = make_mock_tier_res(tier_mock);

    std::string user_content;
    auto capture_loop = [](LoopContext& ctx, void* ud) {
        auto* content = static_cast<std::string*>(ud);
        for (const auto& m : ctx.messages) {
            if (m.role == "user") {
                *content = m.content;
            }
        }
        ctx.state = AgentState::COMPLETE;
        Message m;
        m.role = "assistant";
        m.content = "done";
        ctx.messages.push_back(std::move(m));
    };

    DelegationManager mgr(capture_loop, &user_content, tier_res);
    LoopContext parent;

    mgr.execute_delegation(parent, "eng", "build it");
    REQUIRE(user_content.find("entropic.complete") !=
            std::string::npos);
}

TEST_CASE("extract_summary prefers explicit completion summary",
          "[delegation]") {
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    auto loop_fn = [](LoopContext& ctx, void* /*ud*/) {
        ctx.metadata["explicit_completion_summary"] = "Explicit done";
        Message m;
        m.role = "assistant";
        m.content = "This is the assistant content";
        ctx.messages.push_back(std::move(m));
        ctx.state = AgentState::COMPLETE;
    };

    int dummy = 0;
    DelegationManager mgr(loop_fn, &dummy, tier_res);
    LoopContext parent;

    auto result = mgr.execute_delegation(parent, "eng", "task");
    REQUIRE(result.summary == "Explicit done");
}

TEST_CASE("extract_summary falls back to last assistant message",
          "[delegation]") {
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    auto loop_fn = [](LoopContext& ctx, void* /*ud*/) {
        Message m;
        m.role = "assistant";
        m.content = "Fallback summary";
        ctx.messages.push_back(std::move(m));
        ctx.state = AgentState::COMPLETE;
    };

    int dummy = 0;
    DelegationManager mgr(loop_fn, &dummy, tier_res);
    LoopContext parent;

    auto result = mgr.execute_delegation(parent, "eng", "task");
    REQUIRE(result.summary == "Fallback summary");
}

TEST_CASE("extract_summary returns placeholder when no response",
          "[delegation]") {
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    auto loop_fn = [](LoopContext& ctx, void* /*ud*/) {
        ctx.state = AgentState::COMPLETE;
    };

    int dummy = 0;
    DelegationManager mgr(loop_fn, &dummy, tier_res);
    LoopContext parent;

    auto result = mgr.execute_delegation(parent, "eng", "task");
    REQUIRE(result.summary == "(No response from delegate)");
}

TEST_CASE("todo list saved and restored across delegation",
          "[delegation]") {
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    struct TodoState {
        std::string saved;
        bool fresh_installed = false;
        bool restored = false;
        std::string restored_with;
    };

    TodoState todo;

    auto loop_fn = [](LoopContext& ctx, void* /*ud*/) {
        ctx.state = AgentState::COMPLETE;
        Message m;
        m.role = "assistant";
        m.content = "done";
        ctx.messages.push_back(std::move(m));
    };

    int dummy = 0;
    DelegationManager mgr(loop_fn, &dummy, tier_res);

    TodoCallbacks tc;
    tc.save = [](void* ud) -> std::string {
        static_cast<TodoState*>(ud)->saved = "parent_todos";
        return "parent_todos";
    };
    tc.install_fresh = [](void* ud) {
        static_cast<TodoState*>(ud)->fresh_installed = true;
    };
    tc.restore = [](const std::string& s, void* ud) {
        auto* st = static_cast<TodoState*>(ud);
        st->restored = true;
        st->restored_with = s;
    };
    tc.user_data = &todo;
    mgr.set_todo_callbacks(tc);

    LoopContext parent;
    mgr.execute_delegation(parent, "eng", "task");

    REQUIRE(todo.fresh_installed);
    REQUIRE(todo.restored);
    REQUIRE(todo.restored_with == "parent_todos");
}

TEST_CASE("pipeline executes stages sequentially",
          "[delegation]") {
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    std::vector<std::string> executed_tiers;
    auto loop_fn = [](LoopContext& ctx, void* ud) {
        auto* tiers = static_cast<std::vector<std::string>*>(ud);
        tiers->push_back(ctx.locked_tier.empty()
            ? "resolved" : ctx.locked_tier);
        ctx.state = AgentState::COMPLETE;
        Message m;
        m.role = "assistant";
        m.content = "stage done";
        ctx.messages.push_back(std::move(m));
    };

    DelegationManager mgr(loop_fn, &executed_tiers, tier_res);
    LoopContext parent;

    auto result = mgr.execute_pipeline(
        parent, {"eng", "qa"}, "build and test");

    REQUIRE(result.success);
    REQUIRE(executed_tiers.size() == 2);
}

TEST_CASE("delegation result captures turn count",
          "[delegation]") {
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    MockChildLoop loop_mock;
    loop_mock.complete = true;
    loop_mock.response = "done in 3 turns";

    DelegationManager mgr(mock_run_child, &loop_mock, tier_res);
    LoopContext parent;

    auto result = mgr.execute_delegation(parent, "eng", "task");
    REQUIRE(result.success);
    REQUIRE(result.turns_used == 3);
    REQUIRE(result.target_tier == "eng");
}
