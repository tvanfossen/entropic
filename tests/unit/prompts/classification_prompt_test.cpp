/**
 * @file test_classification_prompt.cpp
 * @brief BDD tests for classification prompt builder.
 * @version 1.8.1
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <entropic/prompts/classification.h>

using namespace entropic::prompts;

SCENARIO("Build classification prompt with tiers", "[prompts][classification]") {
    GIVEN("Three tiers with focus and examples") {
        std::vector<TierDescriptor> tiers = {
            {"lead", {"triage", "delegate"}, {"Help me", "Hello"}},
            {"eng", {"write code"}, {"Write a function"}},
            {"qa", {"test code"}, {}},
        };

        WHEN("build_classification_prompt is called") {
            auto result = build_classification_prompt(
                tiers, "Build a login page");

            THEN("it starts with the instruction") {
                REQUIRE(result.find(
                    "Classify the message. Reply with the number only.")
                        == 0);
            }

            THEN("it contains tier descriptions") {
                REQUIRE(result.find("1 = LEAD:") != std::string::npos);
                REQUIRE(result.find("2 = ENG:") != std::string::npos);
                REQUIRE(result.find("3 = QA:") != std::string::npos);
            }

            THEN("it ends with the message and trailing space") {
                auto expected = "\"Build a login page\" -> ";
                REQUIRE(result.find(expected) != std::string::npos);
                // Last character is a space (for digit output)
                REQUIRE(result.back() == ' ');
            }
        }
    }
}

SCENARIO("Interleave examples round-robin", "[prompts][classification]") {
    GIVEN("Tiers with unequal example counts") {
        std::vector<TierDescriptor> tiers = {
            {"t1", {"f1"}, {"a1", "a2", "a3"}},
            {"t2", {"f2"}, {"b1"}},
        };

        WHEN("interleave_examples is called") {
            auto lines = interleave_examples(tiers);

            THEN("examples are round-robin interleaved") {
                REQUIRE(lines.size() == 4);
                REQUIRE(lines[0] == "\"a1\" -> 1");
                REQUIRE(lines[1] == "\"b1\" -> 2");
                REQUIRE(lines[2] == "\"a2\" -> 1");
                REQUIRE(lines[3] == "\"a3\" -> 1");
            }
        }
    }

    GIVEN("Tiers with no examples") {
        std::vector<TierDescriptor> tiers = {
            {"t1", {"f1"}, {}},
            {"t2", {"f2"}, {}},
        };

        WHEN("interleave_examples is called") {
            auto lines = interleave_examples(tiers);

            THEN("result is empty") {
                REQUIRE(lines.empty());
            }
        }
    }
}

SCENARIO("History and recent_tiers in classification prompt",
         "[prompts][classification]") {
    std::vector<TierDescriptor> tiers = {
        {"lead", {"triage"}, {}},
    };

    GIVEN("Non-empty history") {
        std::vector<std::string> history = {"msg1", "msg2", "msg3"};

        WHEN("prompt is built") {
            auto result = build_classification_prompt(
                tiers, "test", history, {});

            THEN("it contains Recent messages") {
                REQUIRE(result.find("Recent messages: msg1 | msg2 | msg3")
                        != std::string::npos);
            }
        }
    }

    GIVEN("History with more than 5 messages") {
        std::vector<std::string> history = {
            "m1", "m2", "m3", "m4", "m5", "m6", "m7", "m8"};

        WHEN("prompt is built") {
            auto result = build_classification_prompt(
                tiers, "test", history, {});

            THEN("only last 5 are included") {
                REQUIRE(result.find("m1") == std::string::npos);
                REQUIRE(result.find("m4") != std::string::npos);
                REQUIRE(result.find("m8") != std::string::npos);
            }
        }
    }

    GIVEN("Non-empty recent_tiers") {
        std::vector<std::string> recent = {"lead", "eng"};

        WHEN("prompt is built") {
            auto result = build_classification_prompt(
                tiers, "test", {}, recent);

            THEN("it contains Recent tiers with arrows") {
                REQUIRE(result.find("Recent tiers: lead -> eng")
                        != std::string::npos);
            }
        }
    }

    GIVEN("Empty history and recent_tiers") {
        WHEN("prompt is built") {
            auto result = build_classification_prompt(
                tiers, "test", {}, {});

            THEN("neither section appears") {
                REQUIRE(result.find("Recent messages:")
                        == std::string::npos);
                REQUIRE(result.find("Recent tiers:")
                        == std::string::npos);
            }
        }
    }
}
