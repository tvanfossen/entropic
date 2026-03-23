/**
 * @file test_hook_stub.cpp
 * @brief Tests for entropic_register_hook() stub behavior.
 * @version 1.8.9
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>

/**
 * @brief Dummy hook callback for testing.
 * @param hook_point Unused.
 * @param context_json Unused.
 * @param user_data Unused.
 * @return Always 0.
 * @callback
 * @version 1.8.9
 */
static int dummy_callback(
    entropic_hook_point_t hook_point,
    const char* context_json,
    void* user_data) {
    (void)hook_point;
    (void)context_json;
    (void)user_data;
    return 0;
}

/**
 * @brief Sentinel non-NULL handle for stub testing.
 * @return Non-NULL handle that is never dereferenced.
 * @internal
 * @version 1.8.9
 */
static entropic_handle_t fake_handle() {
    static int sentinel = 0;
    return reinterpret_cast<entropic_handle_t>(&sentinel);
}

SCENARIO("Hook registration returns NOT_IMPLEMENTED", "[api][hooks]") {
    GIVEN("a valid engine handle") {
        entropic_handle_t h = fake_handle();

        WHEN("entropic_register_hook is called with PRE_GENERATE") {
            entropic_error_t err = entropic_register_hook(
                h, ENTROPIC_HOOK_PRE_GENERATE, dummy_callback, nullptr);

            THEN("it returns ENTROPIC_ERROR_NOT_IMPLEMENTED") {
                REQUIRE(err == ENTROPIC_ERROR_NOT_IMPLEMENTED);
            }
        }
    }
}

SCENARIO("Hook registration with null handle", "[api][hooks]") {
    GIVEN("a null handle") {
        WHEN("entropic_register_hook(NULL, ...) is called") {
            entropic_error_t err = entropic_register_hook(
                nullptr, ENTROPIC_HOOK_PRE_GENERATE,
                dummy_callback, nullptr);

            THEN("it returns ENTROPIC_ERROR_INVALID_HANDLE") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_HANDLE);
            }
        }
    }
}

SCENARIO("Hook registration with null callback", "[api][hooks]") {
    GIVEN("a valid handle") {
        entropic_handle_t h = fake_handle();

        WHEN("entropic_register_hook with NULL callback is called") {
            entropic_error_t err = entropic_register_hook(
                h, ENTROPIC_HOOK_PRE_GENERATE, nullptr, nullptr);

            THEN("it returns ENTROPIC_ERROR_INVALID_ARGUMENT") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}

SCENARIO("All hook points return NOT_IMPLEMENTED", "[api][hooks]") {
    GIVEN("a valid engine handle") {
        entropic_handle_t h = fake_handle();

        WHEN("entropic_register_hook is called for each hook point") {
            entropic_hook_point_t points[] = {
                ENTROPIC_HOOK_PRE_GENERATE,
                ENTROPIC_HOOK_POST_GENERATE,
                ENTROPIC_HOOK_ON_STREAM_TOKEN,
                ENTROPIC_HOOK_PRE_TOOL_CALL,
                ENTROPIC_HOOK_POST_TOOL_CALL,
                ENTROPIC_HOOK_ON_LOOP_ITERATION,
                ENTROPIC_HOOK_ON_STATE_CHANGE,
                ENTROPIC_HOOK_ON_ERROR,
                ENTROPIC_HOOK_ON_DELEGATE,
                ENTROPIC_HOOK_ON_DELEGATE_COMPLETE,
                ENTROPIC_HOOK_ON_CONTEXT_ASSEMBLE,
                ENTROPIC_HOOK_ON_PRE_COMPACT,
                ENTROPIC_HOOK_ON_POST_COMPACT,
                ENTROPIC_HOOK_ON_MODEL_LOAD,
                ENTROPIC_HOOK_ON_MODEL_UNLOAD,
                ENTROPIC_HOOK_ON_PERMISSION_CHECK,
            };

            THEN("every call returns ENTROPIC_ERROR_NOT_IMPLEMENTED") {
                for (auto hp : points) {
                    entropic_error_t err = entropic_register_hook(
                        h, hp, dummy_callback, nullptr);
                    REQUIRE(err == ENTROPIC_ERROR_NOT_IMPLEMENTED);
                }
            }
        }
    }
}
