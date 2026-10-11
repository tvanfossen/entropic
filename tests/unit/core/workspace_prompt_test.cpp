// SPDX-License-Identifier: Apache-2.0
/**
 * @file workspace_prompt_test.cpp
 * @brief gh#179 (v2.14.0): a session bound to a workspace is seeded with
 *        that workspace's prompts; every other session is untouched.
 *
 * One resident model serving several repositories gave every session the
 * same handle-wide app_context. The engine now holds per-workspace prompts
 * (assembled by the facade: constitution + the workspace's app_context +
 * tier identity) and a session→workspace binding, and resolves them at the
 * three places a system prompt enters a conversation: first-turn seeding,
 * tier seeding (run_turn_as), and a delegated child's context.
 *
 * CPU-only against MockInference: seeding is engine state.
 *
 * @version 2.14.0
 */

#include <entropic/core/engine.h>
#include "mock_inference.h"
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace entropic;
using namespace entropic::test;

namespace {

/// @brief The first (system) message a session was seeded with.
std::string seeded(const AgentEngine& engine, const std::string& key) {
    auto msgs = engine.messages_for(key);
    return (!msgs.empty() && msgs.front().role == "system") ? msgs.front().content : "";
}

}  // namespace

SCENARIO("gh#179: a workspace-bound session is seeded with its workspace's prompts",
         "[engine][gh179][workspace][2.14.0]") {
    MockInference mock;
    auto iface = make_mock_interface(mock);
    LoopConfig lc;
    CompactionConfig cc;
    AgentEngine engine(iface, lc, cc);
    engine.set_system_prompt("HANDLE-DEFAULT");
    ChildContextInfo eng;
    eng.valid = true;
    eng.system_prompt = "HANDLE-ENG";
    engine.set_tier_info("eng", eng);
    engine.set_handoff_rules({});  // wires internal tier resolution

    WorkspacePrompts repo_a;
    repo_a.default_prompt = "A-DEFAULT";
    repo_a.tier_prompts["eng"] = "A-ENG";
    engine.set_workspace_prompts("A", repo_a);
    engine.bind_session_workspace("in-a", "A");

    WHEN("a bound session and an unbound one each take a first turn") {
        engine.set_active_session("in-a");
        engine.run_turn("hello");
        engine.set_active_session("elsewhere");
        engine.run_turn("hello");
        THEN("each gets its own context") {
            CHECK(seeded(engine, "in-a") == "A-DEFAULT");
            CHECK(seeded(engine, "elsewhere") == "HANDLE-DEFAULT");
        }
    }
    WHEN("a bound session's first turn names a tier") {
        engine.bind_session_workspace("in-a-eng", "A");
        engine.set_active_session("in-a-eng");
        engine.run_turn_as("eng", "hello");
        THEN("it is seeded with the workspace's prompt for that tier") {
            CHECK(seeded(engine, "in-a-eng") == "A-ENG");
        }
    }
    WHEN("a bound session delegates to a tier") {
        engine.set_active_session("in-a");
        const auto& tri = engine.tier_resolution();
        REQUIRE(tri.resolve_tier != nullptr);
        auto child = tri.resolve_tier("eng", tri.user_data);
        THEN("the child's context carries the workspace's prompt") {
            CHECK(child.valid);
            CHECK(child.system_prompt == "A-ENG");
        }
        AND_WHEN("an unbound session delegates") {
            engine.set_active_session("elsewhere");
            auto plain = tri.resolve_tier("eng", tri.user_data);
            THEN("it keeps the handle's tier prompt") {
                CHECK(plain.system_prompt == "HANDLE-ENG");
            }
        }
    }
    WHEN("the workspace's prompts are cleared before a bound session starts") {
        engine.clear_workspace_prompts("A");
        engine.bind_session_workspace("late", "A");
        engine.set_active_session("late");
        engine.run_turn("hello");
        THEN("the session falls back to the handle's prompt") {
            CHECK(seeded(engine, "late") == "HANDLE-DEFAULT");
        }
    }
}
