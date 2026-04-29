// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_api_version.cpp
 * @brief Tests for entropic_api_version() and entropic_version().
 *
 * The library version assertion reads the repo-root VERSION file at
 * test time (v2.1.2 #4). Single source of truth: CMakeLists.txt also
 * reads VERSION at configure time, so the runtime value of
 * ``entropic_version()`` should equal the contents of VERSION on every
 * build. If they diverge, either the build was stale or the VERSION
 * read paths got out of sync — both are real bugs the test should
 * catch. ``ENTROPIC_VERSION_FILE_PATH`` is injected by the
 * ``entropic-api-tests`` target via ``target_compile_definitions``.
 *
 * @version 2.1.2
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

/**
 * @brief Read the repo-root VERSION file and strip trailing whitespace.
 * @return The version string declared in VERSION, or empty on read error.
 * @internal
 * @version 2.1.2
 */
std::string read_canonical_version() {
    std::ifstream f(ENTROPIC_VERSION_FILE_PATH);
    if (!f.is_open()) { return {}; }
    std::stringstream ss;
    ss << f.rdbuf();
    auto v = ss.str();
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' ||
                          v.back() == ' ' || v.back() == '\t')) {
        v.pop_back();
    }
    return v;
}

} // namespace

SCENARIO("API version is queryable at runtime", "[api][version]") {
    GIVEN("the entropic library is loaded") {
        WHEN("entropic_api_version() is called") {
            int version = entropic_api_version();

            THEN("it returns 2") {
                REQUIRE(version == 2);
            }
        }
    }
}

SCENARIO("Library version matches the canonical VERSION file",
         "[api][version]") {
    GIVEN("the entropic library is loaded") {
        WHEN("entropic_version() is called") {
            const char* ver = entropic_version();

            THEN("it returns the version declared in the VERSION file") {
                REQUIRE(ver != nullptr);
                auto canonical = read_canonical_version();
                REQUIRE_FALSE(canonical.empty());
                REQUIRE(std::string(ver) == canonical);
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
