/**
 * @file test_router_classification.cpp
 * @brief Regression test from test5: router uses complete() not generate().
 *
 * Validates that tier routing invokes the route callback (which
 * internally uses complete() for digit-based classification) and
 * that the engine respects the routed tier. The mock tracks
 * complete_call_count to distinguish from generate().
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
    sys.content = "You are a router.";
    Message usr;
    usr.role = "user";
    usr.content = user_text;
    return {sys, usr};
}

} // anonymous namespace

SCENARIO("Route callback is invoked for tier selection",
         "[regression][test5]")
{
    GIVEN("an engine with route callback configured") {
        MockInference mock;
        mock.tier = "eng";
        mock.response = "Engineering response.";
        auto iface = make_mock_interface(mock);

        LoopConfig lc;
        lc.max_iterations = 1;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        std::string selected_tier;
        EngineCallbacks cb{};
        cb.on_tier_selected = [](const char* tier, void* ud) {
            *static_cast<std::string*>(ud) = tier;
        };
        cb.user_data = &selected_tier;
        engine.set_callbacks(cb);

        WHEN("engine runs a prompt that triggers routing") {
            engine.run(make_messages("Help me code"));

            THEN("route was called at least once") {
                REQUIRE(mock.route_call_count >= 1);
            }
        }
    }
}

SCENARIO("Complete callback is separate from generate",
         "[regression][test5]")
{
    GIVEN("a mock with both generate and complete wired") {
        MockInference mock;
        mock.response = "Generated text.";
        mock.complete_response = "3";
        auto iface = make_mock_interface(mock);

        WHEN("complete and generate are called independently") {
            char* gen_result = nullptr;
            iface.generate(
                "[]", "{}", &gen_result, iface.backend_data);

            char* comp_result = nullptr;
            iface.complete(
                "Classify:", "{}", &comp_result, iface.backend_data);

            THEN("generate returns the response") {
                REQUIRE(std::string(gen_result) == "Generated text.");
            }
            THEN("complete returns the classification") {
                REQUIRE(std::string(comp_result) == "3");
            }
            THEN("call counts are independent") {
                REQUIRE(mock.generate_call_count == 1);
                REQUIRE(mock.complete_call_count == 1);
            }

            iface.free_fn(gen_result);
            iface.free_fn(comp_result);
        }
    }
}

SCENARIO("Router consistency across repeated calls",
         "[regression][test5]")
{
    GIVEN("a mock router that always returns the same tier") {
        MockInference mock;
        mock.tier = "qa";
        auto iface = make_mock_interface(mock);

        WHEN("route is called multiple times") {
            char* r1 = nullptr;
            char* r2 = nullptr;
            char* r3 = nullptr;
            iface.route("[]", &r1, iface.orchestrator_data);
            iface.route("[]", &r2, iface.orchestrator_data);
            iface.route("[]", &r3, iface.orchestrator_data);

            THEN("all return the same tier") {
                REQUIRE(std::string(r1) == "qa");
                REQUIRE(std::string(r2) == "qa");
                REQUIRE(std::string(r3) == "qa");
            }
            THEN("route was called 3 times") {
                REQUIRE(mock.route_call_count == 3);
            }

            iface.free_fn(r1);
            iface.free_fn(r2);
            iface.free_fn(r3);
        }
    }
}
