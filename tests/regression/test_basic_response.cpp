// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_basic_response.cpp
 * @brief Regression test from test1: basic generation flow.
 *
 * Validates response cleanliness — no special tokens (<|im_start|>,
 * </s>, etc.) leak into final output. Uses mock inference with
 * scripted clean responses.
 *
 * @version 1.10.1
 */

#include <entropic/core/engine.h>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mock_inference.h"

using namespace entropic;
using namespace entropic::test;

namespace {

/**
 * @brief Build a minimal message list with system + user message.
 * @param user_text User prompt text.
 * @return Message vector.
 * @utility
 * @version 1.10.1
 */
std::vector<Message> make_messages(const std::string& user_text) {
    Message sys;
    sys.role = "system";
    sys.content = "You are a helpful assistant.";
    Message usr;
    usr.role = "user";
    usr.content = user_text;
    return {sys, usr};
}

/**
 * @brief Check that content has no special token markers.
 * @param content Response text to validate.
 * @return true if clean.
 * @utility
 * @version 1.10.1
 */
bool is_clean(const std::string& content) {
    static const char* markers[] = {
        "<|im_start|>", "<|im_end|>", "</s>", "<s>",
        "<|endoftext|>", "<|assistant|>", "<|user|>",
    };
    for (const auto* m : markers) {
        if (content.find(m) != std::string::npos) {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

SCENARIO("Basic response contains no special tokens",
         "[regression][test1]")
{
    GIVEN("an engine with a clean mock response") {
        MockInference mock;
        mock.response = "The capital of France is Paris.";
        auto iface = make_mock_interface(mock);

        LoopConfig lc;
        lc.max_iterations = 1;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        WHEN("the engine processes a simple prompt") {
            auto result = engine.run(
                make_messages("What is the capital of France?"));

            THEN("the response is present and clean") {
                REQUIRE(result.size() >= 3);
                auto& assistant = result.back();
                REQUIRE(assistant.role == "assistant");
                REQUIRE(is_clean(assistant.content));
                REQUIRE(assistant.content.find("Paris")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Empty response handled gracefully",
         "[regression][test1]")
{
    GIVEN("an engine with an empty mock response") {
        MockInference mock;
        mock.response = "";
        auto iface = make_mock_interface(mock);

        LoopConfig lc;
        lc.max_iterations = 1;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        WHEN("the engine processes a prompt") {
            auto result = engine.run(
                make_messages("Hello"));

            THEN("engine completes without crash") {
                REQUIRE(result.size() >= 2);
            }
        }
    }
}

SCENARIO("Generate is called exactly once per iteration",
         "[regression][test1]")
{
    GIVEN("a single-iteration engine") {
        MockInference mock;
        mock.response = "Simple answer.";
        auto iface = make_mock_interface(mock);

        LoopConfig lc;
        lc.max_iterations = 1;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        WHEN("run completes") {
            engine.run(make_messages("Hi"));

            THEN("generate was called once") {
                REQUIRE(mock.generate_call_count == 1);
            }
        }
    }
}
