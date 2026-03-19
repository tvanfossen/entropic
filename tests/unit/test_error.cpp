/**
 * @file test_error.cpp
 * @brief BDD tests for entropic_error_t and related functions.
 * @version 1.8.0
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <entropic/types/error.h>
#include <cstring>

SCENARIO("Error codes have human-readable names", "[error][types]") {
    GIVEN("Each defined error code") {
        auto code = GENERATE(
            ENTROPIC_OK,
            ENTROPIC_ERROR_INVALID_ARGUMENT,
            ENTROPIC_ERROR_INVALID_CONFIG,
            ENTROPIC_ERROR_INVALID_STATE,
            ENTROPIC_ERROR_MODEL_NOT_FOUND,
            ENTROPIC_ERROR_LOAD_FAILED,
            ENTROPIC_ERROR_GENERATE_FAILED,
            ENTROPIC_ERROR_TOOL_NOT_FOUND,
            ENTROPIC_ERROR_PERMISSION_DENIED,
            ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH,
            ENTROPIC_ERROR_PLUGIN_LOAD_FAILED,
            ENTROPIC_ERROR_TIMEOUT,
            ENTROPIC_ERROR_CANCELLED,
            ENTROPIC_ERROR_OUT_OF_MEMORY,
            ENTROPIC_ERROR_IO,
            ENTROPIC_ERROR_INTERNAL
        );

        WHEN("entropic_error_name is called") {
            const char* name = entropic_error_name(code);

            THEN("it returns a non-null, non-empty string") {
                REQUIRE(name != nullptr);
                REQUIRE(std::strlen(name) > 0);
            }

            THEN("the string starts with ENTROPIC_") {
                REQUIRE(std::strncmp(name, "ENTROPIC_", 9) == 0);
            }
        }
    }
}

SCENARIO("ENTROPIC_OK is zero", "[error][types]") {
    GIVEN("The success code") {
        THEN("it evaluates to false in boolean context") {
            REQUIRE(ENTROPIC_OK == 0);
        }
    }
}

SCENARIO("entropic_last_error returns empty string initially", "[error][types]") {
    GIVEN("A NULL handle (pre-creation)") {
        WHEN("entropic_last_error is called") {
            const char* msg = entropic_last_error(nullptr);

            THEN("it returns an empty string, not NULL") {
                REQUIRE(msg != nullptr);
                REQUIRE(std::strlen(msg) == 0);
            }
        }
    }
}

SCENARIO("Error callback registration rejects NULL handle", "[error][types]") {
    GIVEN("A NULL handle") {
        WHEN("setting an error callback") {
            auto result = entropic_set_error_callback(nullptr, nullptr, nullptr);

            THEN("it returns INVALID_ARGUMENT") {
                REQUIRE(result == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}
