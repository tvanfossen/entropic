// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_generation_params.cpp
 * @brief Tests for GenerationParams struct defaults.
 * @version 1.8.2
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/types/config.h>

SCENARIO("GenerationParams defaults", "[params][defaults]") {
    GIVEN("a default-constructed GenerationParams") {
        entropic::GenerationParams params;

        THEN("defaults match specification") {
            REQUIRE(params.max_tokens == 4096);
            REQUIRE(params.temperature == 0.7f);
            REQUIRE(params.top_p == 0.9f);
            REQUIRE(params.top_k == 40);
            REQUIRE(params.repeat_penalty == 1.1f);
            REQUIRE(params.reasoning_budget == -1);
            REQUIRE(params.enable_thinking == true);
            REQUIRE(params.grammar.empty());
            REQUIRE(params.stop.empty());
            REQUIRE(params.logprobs == 0);
        }
    }
}

SCENARIO("ModelState enum values", "[types][enums]") {
    THEN("ModelState maps to C enum values") {
        REQUIRE(static_cast<int>(entropic::ModelState::COLD) == 0);
        REQUIRE(static_cast<int>(entropic::ModelState::WARM) == 1);
        REQUIRE(static_cast<int>(entropic::ModelState::ACTIVE) == 2);
    }
}
