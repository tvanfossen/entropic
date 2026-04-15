// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_logging.cpp
 * @brief BDD tests for spdlog initialization pattern.
 * @version 1.8.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/types/logging.h>

SCENARIO("Logging initialization is idempotent", "[logging][types]") {
    GIVEN("The logging subsystem") {
        WHEN("init is called multiple times") {
            entropic::log::init(spdlog::level::info);
            entropic::log::init(spdlog::level::debug);
            entropic::log::init(spdlog::level::warn);

            THEN("no crash occurs and system remains functional") {
                auto logger = entropic::log::get("test-idempotent");
                REQUIRE(logger != nullptr);
            }
        }
    }
}

SCENARIO("Named loggers are created on demand", "[logging][types]") {
    GIVEN("The logging subsystem is initialized") {
        entropic::log::init(spdlog::level::info);

        WHEN("a named logger is requested") {
            auto logger = entropic::log::get("test-named");

            THEN("it returns a non-null logger") {
                REQUIRE(logger != nullptr);
            }

            THEN("the logger has the requested name") {
                REQUIRE(logger->name() == "test-named");
            }
        }
    }
}

SCENARIO("Repeated get() returns the same logger", "[logging][types]") {
    GIVEN("A named logger has been created") {
        entropic::log::init(spdlog::level::info);
        auto first = entropic::log::get("test-same");

        WHEN("the same name is requested again") {
            auto second = entropic::log::get("test-same");

            THEN("it returns the same logger instance") {
                REQUIRE(first.get() == second.get());
            }
        }
    }
}
