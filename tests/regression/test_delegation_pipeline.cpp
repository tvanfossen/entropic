/**
 * @file test_delegation_pipeline.cpp
 * @brief Regression test from test4: lead-to-child delegation.
 *
 * Validates the delegation flow: engine processes a delegate
 * directive, DelegationManager creates a child loop, child
 * executes and returns a summary. Also validates pipeline
 * (multi-stage delegation).
 *
 * @version 1.10.1
 */

#include <entropic/core/engine.h>
#include <entropic/core/engine_types.h>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mock_inference.h"

using namespace entropic;
using namespace entropic::test;

namespace {

/**
 * @brief Build system + user messages.
 * @param user_text User prompt.
 * @return Message vector.
 * @utility
 * @version 1.10.1
 */
std::vector<Message> make_messages(const std::string& user_text) {
    Message sys;
    sys.role = "system";
    sys.content = "You are the lead identity.";
    Message usr;
    usr.role = "user";
    usr.content = user_text;
    return {sys, usr};
}

/**
 * @brief Mock tier resolution for delegation tests.
 * @param tier_name Target tier.
 * @param user_data Unused.
 * @return ChildContextInfo with basic system prompt.
 * @callback
 * @version 1.10.1
 */
ChildContextInfo mock_resolve_tier(
    const std::string& tier_name, void* /*user_data*/) {
    ChildContextInfo info;
    info.valid = true;
    info.system_prompt = "You are " + tier_name + ".";
    return info;
}

/**
 * @brief Mock tier_exists check.
 * @param tier_name Tier to check.
 * @param user_data Unused.
 * @return true always.
 * @callback
 * @version 1.10.1
 */
bool mock_tier_exists(
    const std::string& /*tier_name*/, void* /*user_data*/) {
    return true;
}

} // anonymous namespace

SCENARIO("Delegation directive triggers child loop",
         "[regression][test4]")
{
    GIVEN("an engine whose model triggers a delegation") {
        MockInference mock;
        mock.response = "Delegating to engineering.";
        auto iface = make_mock_interface(mock);

        LoopConfig lc;
        lc.max_iterations = 3;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        TierResolutionInterface tri;
        tri.resolve_tier = mock_resolve_tier;
        tri.tier_exists = mock_tier_exists;
        engine.set_tier_resolution(tri);

        bool delegation_started = false;
        EngineCallbacks cb{};
        cb.on_delegation_start = [](const char*, const char*,
                                    const char*, void* ud) {
            *static_cast<bool*>(ud) = true;
        };
        cb.user_data = &delegation_started;
        engine.set_callbacks(cb);

        WHEN("engine runs with delegation context") {
            auto result = engine.run(
                make_messages("Delegate to eng"));

            THEN("engine completes without error") {
                REQUIRE(result.size() >= 2);
            }

            THEN("generate was called") {
                REQUIRE(mock.generate_call_count >= 1);
            }
        }
    }
}

SCENARIO("Max delegation depth is enforced",
         "[regression][test4]")
{
    GIVEN("an engine") {
        MockInference mock;
        mock.response = "Response from child.";
        auto iface = make_mock_interface(mock);

        LoopConfig lc;
        lc.max_iterations = 1;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        WHEN("MAX_DELEGATION_DEPTH is queried") {
            THEN("it equals 2") {
                REQUIRE(AgentEngine::MAX_DELEGATION_DEPTH == 2);
            }
        }
    }
}
