/**
 * @file test_hook_stub.cpp
 * @brief Tests for entropic_register_hook() and entropic_deregister_hook().
 *
 * Tests the C API surface for hook registration/deregistration.
 * Full dispatch tests are in tests/unit/core/test_hook_registry.cpp.
 *
 * @version 1.9.1
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>
#include <entropic/core/hook_registry.h>

/**
 * @brief Dummy hook callback for testing.
 * @param hook_point Unused.
 * @param context_json Unused.
 * @param modified_json Set to NULL.
 * @param user_data Unused.
 * @return Always 0.
 * @callback
 * @version 1.9.1
 */
static int dummy_callback(
    entropic_hook_point_t hook_point,
    const char* context_json,
    char** modified_json,
    void* user_data) {
    (void)hook_point;
    (void)context_json;
    (void)user_data;
    *modified_json = nullptr;
    return 0;
}

/**
 * @brief Create a handle backed by a real HookRegistry.
 * @param reg HookRegistry to use as handle.
 * @return Handle cast from HookRegistry pointer.
 * @internal
 * @version 1.9.1
 */
static entropic_handle_t make_handle(entropic::HookRegistry& reg) {
    return reinterpret_cast<entropic_handle_t>(&reg);
}

SCENARIO("Hook registration with null handle", "[api][hooks]") {
    GIVEN("a null handle") {
        WHEN("entropic_register_hook(NULL, ...) is called") {
            entropic_error_t err = entropic_register_hook(
                nullptr, ENTROPIC_HOOK_PRE_GENERATE,
                dummy_callback, nullptr, 0);

            THEN("it returns ENTROPIC_ERROR_INVALID_HANDLE") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("Hook registration with null callback", "[api][hooks]") {
    GIVEN("a valid handle") {
        entropic::HookRegistry reg;
        auto h = make_handle(reg);

        WHEN("entropic_register_hook with NULL callback") {
            entropic_error_t err = entropic_register_hook(
                h, ENTROPIC_HOOK_PRE_GENERATE, nullptr, nullptr, 0);

            THEN("it returns ENTROPIC_ERROR_INVALID_ARGUMENT") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("Hook registration succeeds via C API", "[api][hooks]") {
    GIVEN("a valid handle with HookRegistry") {
        entropic::HookRegistry reg;
        auto h = make_handle(reg);

        WHEN("entropic_register_hook is called") {
            entropic_error_t err = entropic_register_hook(
                h, ENTROPIC_HOOK_PRE_GENERATE,
                dummy_callback, nullptr, 0);

            THEN("it returns ENTROPIC_OK") {
                REQUIRE(err == ENTROPIC_OK);
            }
            THEN("the hook is registered") {
                REQUIRE(reg.hook_count(ENTROPIC_HOOK_PRE_GENERATE) == 1);
            }
        }
    }
}

SCENARIO("Hook deregistration via C API", "[api][hooks]") {
    GIVEN("a registered hook") {
        entropic::HookRegistry reg;
        auto h = make_handle(reg);
        entropic_register_hook(h, ENTROPIC_HOOK_ON_ERROR,
                               dummy_callback, nullptr, 0);

        WHEN("entropic_deregister_hook is called") {
            entropic_error_t err = entropic_deregister_hook(
                h, ENTROPIC_HOOK_ON_ERROR, dummy_callback, nullptr);

            THEN("it returns ENTROPIC_OK") {
                REQUIRE(err == ENTROPIC_OK);
            }
            THEN("the hook is removed") {
                REQUIRE(reg.hook_count(ENTROPIC_HOOK_ON_ERROR) == 0);
            }
        }
    }
}

SCENARIO("Hook deregistration with null handle", "[api][hooks]") {
    GIVEN("a null handle") {
        WHEN("entropic_deregister_hook(NULL, ...) is called") {
            entropic_error_t err = entropic_deregister_hook(
                nullptr, ENTROPIC_HOOK_PRE_GENERATE,
                dummy_callback, nullptr);

            THEN("it returns ENTROPIC_ERROR_INVALID_HANDLE") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("Invalid hook point rejected via C API", "[api][hooks]") {
    GIVEN("a valid handle") {
        entropic::HookRegistry reg;
        auto h = make_handle(reg);

        WHEN("registering with ENTROPIC_HOOK_COUNT_") {
            entropic_error_t err = entropic_register_hook(
                h, ENTROPIC_HOOK_COUNT_,
                dummy_callback, nullptr, 0);

            THEN("it returns ENTROPIC_ERROR_INVALID_CONFIG") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_CONFIG);
            }
        }
    }
}

SCENARIO("Hook count sentinel value", "[api][hooks]") {
    GIVEN("the hook enum") {
        THEN("ENTROPIC_HOOK_COUNT_ is 22") {
            REQUIRE(ENTROPIC_HOOK_COUNT_ == 22);
        }
    }
}
