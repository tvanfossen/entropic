/**
 * @file test_api_version.cpp
 * @brief Tests for entropic_api_version() and entropic_version().
 * @version 1.8.9
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>

#include <cstring>

SCENARIO("API version is queryable at runtime", "[api][version]") {
    GIVEN("the entropic library is loaded") {
        WHEN("entropic_api_version() is called") {
            int version = entropic_api_version();

            THEN("it returns 1") {
                REQUIRE(version == 1);
            }
        }
    }
}

SCENARIO("Library version matches build", "[api][version]") {
    GIVEN("the entropic library is loaded") {
        WHEN("entropic_version() is called") {
            const char* ver = entropic_version();

            THEN("it returns the expected version string") {
                REQUIRE(ver != nullptr);
                REQUIRE(std::strcmp(ver, "1.8.9") == 0);
            }

            THEN("the returned pointer is valid for the process lifetime") {
                const char* ver2 = entropic_version();
                REQUIRE(ver == ver2);
            }
        }
    }
}

SCENARIO("Compile-time API version matches runtime", "[api][version]") {
    GIVEN("ENTROPIC_API_VERSION is defined in entropic_config.h") {
        WHEN("compared to entropic_api_version()") {
            THEN("they are equal") {
                REQUIRE(ENTROPIC_API_VERSION == entropic_api_version());
            }
        }
    }
}
