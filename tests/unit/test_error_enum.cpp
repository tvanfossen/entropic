/**
 * @file test_error_enum.cpp
 * @brief Tests for entropic_error_t enum consistency.
 * @version 1.8.9
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/types/error.h>

#include <cstring>
#include <set>

SCENARIO("ENTROPIC_OK is zero", "[api][error]") {
    GIVEN("the entropic_error_t enum") {
        WHEN("ENTROPIC_OK is evaluated") {
            THEN("it equals 0") {
                REQUIRE(ENTROPIC_OK == 0);
            }
        }
    }
}

SCENARIO("All error codes have distinct values", "[api][error]") {
    GIVEN("the entropic_error_t enum") {
        WHEN("all values are collected") {
            std::set<int> values;
            entropic_error_t codes[] = {
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
                ENTROPIC_ERROR_INTERNAL,
                ENTROPIC_ERROR_SERVER_ALREADY_EXISTS,
                ENTROPIC_ERROR_SERVER_NOT_FOUND,
                ENTROPIC_ERROR_CONNECTION_FAILED,
                ENTROPIC_ERROR_INVALID_HANDLE,
                ENTROPIC_ERROR_TOOL_EXECUTION_FAILED,
                ENTROPIC_ERROR_STORAGE_FAILED,
                ENTROPIC_ERROR_IDENTITY_NOT_FOUND,
                ENTROPIC_ERROR_ALREADY_RUNNING,
                ENTROPIC_ERROR_NOT_RUNNING,
                ENTROPIC_ERROR_NOT_IMPLEMENTED,
                ENTROPIC_ERROR_INTERRUPTED,
            };

            for (auto c : codes) {
                values.insert(static_cast<int>(c));
            }

            THEN("no two values are equal") {
                REQUIRE(values.size() == sizeof(codes) / sizeof(codes[0]));
            }
        }
    }
}

SCENARIO("Error names are non-null for all codes", "[api][error]") {
    GIVEN("the entropic_error_t enum") {
        WHEN("entropic_error_name is called for each code") {
            THEN("it returns a non-null, non-empty string") {
                for (int i = 0; i <= static_cast<int>(ENTROPIC_ERROR_INTERRUPTED); ++i) {
                    const char* name = entropic_error_name(
                        static_cast<entropic_error_t>(i));
                    REQUIRE(name != nullptr);
                    REQUIRE(std::strlen(name) > 0);
                }
            }
        }
    }
}

SCENARIO("Unknown error code returns UNKNOWN", "[api][error]") {
    GIVEN("an out-of-range error code") {
        WHEN("entropic_error_name is called with 9999") {
            const char* name = entropic_error_name(
                static_cast<entropic_error_t>(9999));

            THEN("it returns ENTROPIC_ERROR_UNKNOWN") {
                REQUIRE(name != nullptr);
                REQUIRE(std::strcmp(name, "ENTROPIC_ERROR_UNKNOWN") == 0);
            }
        }
    }
}
