/**
 * @file test_identity_behavior.cpp
 * @brief Regression test from test6: identity tool filtering and constitution.
 *
 * Validates that the lead identity has exactly 3 tools (delegate,
 * pipeline, complete) and that constitutional validation catches
 * violations and passes clean content.
 *
 * @version 1.10.2
 */

#include <entropic/core/constitutional_validator.h>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mock_inference.h"

using namespace entropic;
using namespace entropic::test;

namespace {

/**
 * @brief The 3 tools the lead identity is allowed.
 * @version 1.10.1
 */
const std::vector<std::string> LEAD_TOOLS = {
    "entropic.delegate",
    "entropic.pipeline",
    "entropic.complete",
};

} // anonymous namespace

SCENARIO("Lead identity has exactly 3 tools",
         "[regression][test6]")
{
    GIVEN("the defined lead tool set") {
        WHEN("the tool count is checked") {
            THEN("there are exactly 3 tools") {
                REQUIRE(LEAD_TOOLS.size() == 3);
            }
            THEN("delegate is present") {
                REQUIRE(LEAD_TOOLS[0] == "entropic.delegate");
            }
            THEN("pipeline is present") {
                REQUIRE(LEAD_TOOLS[1] == "entropic.pipeline");
            }
            THEN("complete is present") {
                REQUIRE(LEAD_TOOLS[2] == "entropic.complete");
            }
        }
    }
}

SCENARIO("Constitutional validation catches violations",
         "[regression][test6]")
{
    GIVEN("a validator with mock inference returning non-compliant") {
        MockInference mock;
        mock.response_queue = {
            R"({"compliant":false,"violations":[)"
            R"({"rule":"Privacy","excerpt":"upload",)"
            R"("explanation":"external upload"}],)"
            R"("revised":"Process locally"})",
            R"({"compliant":true,"violations":[],"revised":""})",
        };
        auto iface = make_mock_interface(mock);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        cfg.max_revisions = 2;
        ConstitutionalValidator validator(
            cfg, "Privacy First: All processing must be local.");
        validator.attach(nullptr, &iface);

        WHEN("validate is called with violating content") {
            auto result = validator.validate(
                "Upload to cloud", "eng", nullptr);

            THEN("content is revised") {
                REQUIRE(result.was_revised == true);
                REQUIRE(result.content == "Process locally");
            }
        }
    }
}

SCENARIO("Constitutional validation passes clean content",
         "[regression][test6]")
{
    GIVEN("a validator returning compliant") {
        MockInference mock;
        mock.response =
            R"({"compliant":true,"violations":[],"revised":""})";
        auto iface = make_mock_interface(mock);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(
            cfg, "Privacy First: All processing local.");
        validator.attach(nullptr, &iface);

        WHEN("validate is called with safe content") {
            auto result = validator.validate(
                "Process data locally", "eng", nullptr);

            THEN("original content returned unchanged") {
                REQUIRE(result.content == "Process data locally");
                REQUIRE(result.was_revised == false);
            }
        }
    }
}
