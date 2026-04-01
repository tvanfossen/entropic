/**
 * @file test_identity_api.cpp
 * @brief Tests for entropic_load_identity() and entropic_get_identity().
 * @version 1.8.9
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>

#include <cstring>

/**
 * @brief Helper to get a valid (non-NULL) engine handle for testing.
 *
 * entropic_create() currently returns ENTROPIC_ERROR_INTERNAL and sets
 * handle to NULL. For testing NULL-handle guard paths, we use NULL
 * directly. For testing the identity stubs themselves, we need a
 * non-NULL handle. We fabricate one via a sentinel — the stub
 * functions only check for NULL, they don't dereference.
 *
 * @return Sentinel non-NULL handle.
 * @internal
 * @version 1.8.9
 */
static entropic_handle_t fake_handle() {
    static int sentinel = 0;
    return reinterpret_cast<entropic_handle_t>(&sentinel);
}

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

SCENARIO("Load identity with null name", "[api][identity]") {
    GIVEN("a valid handle") {
        entropic_handle_t h = fake_handle();

        WHEN("entropic_load_identity(handle, NULL) is called") {
            entropic_error_t err = entropic_load_identity(h, nullptr);

            THEN("it returns ENTROPIC_ERROR_INVALID_ARGUMENT") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("Load a nonexistent identity (stub behavior)", "[api][identity]") {
    GIVEN("a valid handle with no identities configured") {
        entropic_handle_t h = fake_handle();

        WHEN("entropic_load_identity(handle, \"eng\") is called") {
            entropic_error_t err = entropic_load_identity(h, "eng");

            THEN("it returns ENTROPIC_ERROR_IDENTITY_NOT_FOUND") {
                REQUIRE(err == ENTROPIC_ERROR_IDENTITY_NOT_FOUND);
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

SCENARIO("Get identity with null output pointer", "[api][identity]") {
    GIVEN("a valid handle") {
        entropic_handle_t h = fake_handle();

        WHEN("entropic_get_identity(handle, NULL) is called") {
            entropic_error_t err = entropic_get_identity(h, nullptr);

            THEN("it returns ENTROPIC_ERROR_INVALID_ARGUMENT") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("Get identity with no identity loaded (stub)", "[api][identity]") {
    GIVEN("a valid handle with no identity loaded") {
        entropic_handle_t h = fake_handle();

        WHEN("entropic_get_identity(handle, &json) is called") {
            char* json = nullptr;
            entropic_error_t err = entropic_get_identity(h, &json);

            THEN("it returns ENTROPIC_ERROR_IDENTITY_NOT_FOUND") {
                REQUIRE(err == ENTROPIC_ERROR_IDENTITY_NOT_FOUND);
            }
        }
    }
}
