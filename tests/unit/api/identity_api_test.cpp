// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_identity_api.cpp
 * @brief Tests for entropic_load_identity() and entropic_get_identity().
 * @version 2.0.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>

#include <cstring>

/**
 * @brief RAII wrapper for entropic handle in tests.
 * @internal
 * @version 2.0.0
 */
struct HandleGuard {
    entropic_handle_t h = nullptr;
    HandleGuard() { entropic_create(&h); }
    ~HandleGuard() { entropic_destroy(h); }
    operator entropic_handle_t() const { return h; }
};

SCENARIO("Load identity with null handle", "[api][identity]") {
    GIVEN("a null handle") {
        WHEN("entropic_load_identity(NULL, \"eng\") is called") {
            entropic_error_t err = entropic_load_identity(nullptr, "eng");

            THEN("it returns ENTROPIC_ERROR_INVALID_HANDLE") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("Load identity with null name on unconfigured handle", "[api][identity]") {
    GIVEN("a valid but unconfigured handle") {
        HandleGuard h;

        WHEN("entropic_load_identity(handle, NULL) is called") {
            entropic_error_t err = entropic_load_identity(h, nullptr);

            THEN("it returns ENTROPIC_ERROR_INVALID_STATE (state checked first)") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_STATE);
            }
        }
    }
}

SCENARIO("Load identity on unconfigured handle", "[api][identity]") {
    GIVEN("a valid handle that has not been configured") {
        HandleGuard h;

        WHEN("entropic_load_identity(handle, \"eng\") is called") {
            entropic_error_t err = entropic_load_identity(h, "eng");

            THEN("it returns ENTROPIC_ERROR_INVALID_STATE") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_STATE);
            }
        }
    }
}

SCENARIO("Get identity with null handle", "[api][identity]") {
    GIVEN("a null handle") {
        WHEN("entropic_get_identity(NULL, &json) is called") {
            char* json = nullptr;
            entropic_error_t err = entropic_get_identity(nullptr, &json);

            THEN("it returns ENTROPIC_ERROR_INVALID_HANDLE") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("Get identity with null output on unconfigured handle", "[api][identity]") {
    GIVEN("a valid but unconfigured handle") {
        HandleGuard h;

        WHEN("entropic_get_identity(handle, NULL) is called") {
            entropic_error_t err = entropic_get_identity(h, nullptr);

            THEN("it returns ENTROPIC_ERROR_INVALID_STATE (state checked first)") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_STATE);
            }
        }
    }
}

SCENARIO("Get identity on unconfigured handle", "[api][identity]") {
    GIVEN("a valid handle that has not been configured") {
        HandleGuard h;

        WHEN("entropic_get_identity(handle, &json) is called") {
            char* json = nullptr;
            entropic_error_t err = entropic_get_identity(h, &json);

            THEN("it returns ENTROPIC_ERROR_INVALID_STATE") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_STATE);
            }
        }
    }
}
