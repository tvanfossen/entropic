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

// ── Issue #11 (v2.1.4): pipeline forwards stage output ───

TEST_CASE("pipeline forwards prior stage output as context",
          "[delegation][2.1.4][issue-11]") {
    // Stage 0 records "stage 0 output" as its summary; stage 1's
    // task should now contain "[PRIOR STAGE OUTPUT]" + "stage 0
    // output" — verifying the new forward-carry semantics.
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    struct PipelineCapture {
        int stage = 0;
        std::vector<std::string> seen_user_content;
    } cap;

    auto loop_fn = [](LoopContext& ctx, void* ud) {
        auto* c = static_cast<PipelineCapture*>(ud);
        for (const auto& m : ctx.messages) {
            if (m.role == "user") {
                c->seen_user_content.push_back(m.content);
            }
        }
        Message a;
        a.role = "assistant";
        a.content = "stage " + std::to_string(c->stage)
            + " output";
        ctx.metadata["explicit_completion_summary"] = a.content;
        ctx.messages.push_back(std::move(a));
        ctx.state = AgentState::COMPLETE;
        ++c->stage;
    };

    DelegationManager mgr(loop_fn, &cap, tier_res);
    LoopContext parent;
    auto result = mgr.execute_pipeline(
        parent, {"eng", "qa"}, "do the thing");

    REQUIRE(result.success);
    REQUIRE(cap.seen_user_content.size() == 2);
    // Stage 0 sees the original task without prior output.
    CHECK(cap.seen_user_content[0].find("[PIPELINE CONTEXT]") !=
          std::string::npos);
    CHECK(cap.seen_user_content[0].find("[PRIOR STAGE OUTPUT]") ==
          std::string::npos);
    CHECK(cap.seen_user_content[0].find("do the thing") !=
          std::string::npos);
    // Stage 1 sees the prior stage's output.
    CHECK(cap.seen_user_content[1].find("[PRIOR STAGE OUTPUT]") !=
          std::string::npos);
    CHECK(cap.seen_user_content[1].find("stage 0 output") !=
          std::string::npos);
}

// ── Issue #10 (v2.1.4): coverage_gap propagation ─────────

TEST_CASE("coverage_gap signal propagates from child metadata to "
          "DelegationResult",
          "[delegation][2.1.4][issue-10]") {
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    auto loop_fn = [](LoopContext& ctx, void*) {
        // Simulate dir_complete writing the metadata keys.
        ctx.metadata["explicit_completion_summary"] =
            "researched the docs but need source inspection";
        ctx.metadata["coverage_gap"] = "true";
        ctx.metadata["gap_description"] =
            "docs only describe the API; the bug is in the impl";
        ctx.metadata["suggested_files_json"] =
            R"(["src/foo.cpp","include/foo.h"])";
        Message a;
        a.role = "assistant";
        a.content = "researched the docs but need source inspection";
        ctx.messages.push_back(std::move(a));
        ctx.state = AgentState::COMPLETE;
    };

    DelegationManager mgr(loop_fn, nullptr, tier_res);
    LoopContext parent;

    auto result = mgr.execute_delegation(
        parent, "researcher", "investigate bug");
    REQUIRE(result.success);
    CHECK(result.coverage_gap);
    CHECK(result.gap_description.find("bug is in the impl") !=
          std::string::npos);
    REQUIRE(result.suggested_files.size() == 2);
    CHECK(result.suggested_files[0] == "src/foo.cpp");
    CHECK(result.suggested_files[1] == "include/foo.h");
}

TEST_CASE("coverage_gap absent → result fields stay default",
          "[delegation][2.1.4][issue-10]") {
    MockTierResolution tier_mock;
    auto tier_res = make_mock_tier_res(tier_mock);

    MockChildLoop loop_mock;
    DelegationManager mgr(mock_run_child, &loop_mock, tier_res);
    LoopContext parent;

    auto result = mgr.execute_delegation(parent, "eng", "task");
    CHECK_FALSE(result.coverage_gap);
    CHECK(result.gap_description.empty());
    CHECK(result.suggested_files.empty());
}
