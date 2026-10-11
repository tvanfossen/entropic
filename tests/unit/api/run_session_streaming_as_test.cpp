// SPDX-License-Identifier: Apache-2.0
/**
 * @file run_session_streaming_as_test.cpp
 * @brief gh#205 (v2.14.0): entropic_run_session_streaming_as's argument
 *        contract, on a configured handle with no model loaded.
 *
 * The behaviour under a real tier is exercised end to end by the v2.14.0
 * multi-client serving test on a GPU; this pins every refusal, which needs
 * no model — each is decided before a turn starts.
 *
 * @version 2.14.0
 */
#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>

namespace {

void noop_token(const char*, size_t, void*) {}

/// @brief Owns one handle configured without loading a model.
struct Handle {
    entropic_handle_t h = nullptr;  ///< Handle under test
    Handle() {
        entropic_create(&h);
        REQUIRE(entropic_configure(h, R"({"log_level":"WARN"})") == ENTROPIC_OK);
    }
    ~Handle() { entropic_destroy(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

}  // namespace

SCENARIO("gh#205: streaming under a tier refuses what it cannot run",
         "[api][run_session][gh205][2.14.0]") {
    Handle h;
    GIVEN("a NULL tier") {
        THEN("it is an invalid argument, not a routed turn") {
            CHECK(entropic_run_session_streaming_as(
                      h.h, "s", nullptr, "hi", noop_token, nullptr, nullptr)
                  == ENTROPIC_ERROR_INVALID_ARGUMENT);
        }
    }
    GIVEN("a tier the handle does not have") {
        THEN("it is refused by name, before any turn starts") {
            CHECK(entropic_run_session_streaming_as(
                      h.h, "s", "no_such_tier", "hi", noop_token, nullptr,
                      nullptr)
                  == ENTROPIC_ERROR_IDENTITY_NOT_FOUND);
            const char* err = entropic_last_error(h.h);
            REQUIRE(err != nullptr);
            CHECK(std::string(err).find("no_such_tier") != std::string::npos);
        }
    }
    GIVEN("a NULL input or callback") {
        THEN("both are invalid arguments") {
            CHECK(entropic_run_session_streaming_as(
                      h.h, "s", "lead", nullptr, noop_token, nullptr, nullptr)
                  == ENTROPIC_ERROR_INVALID_ARGUMENT);
            CHECK(entropic_run_session_streaming_as(
                      h.h, "s", "lead", "hi", nullptr, nullptr, nullptr)
                  == ENTROPIC_ERROR_INVALID_ARGUMENT);
        }
    }
    GIVEN("a NULL handle") {
        THEN("it is refused as such") {
            CHECK(entropic_run_session_streaming_as(
                      nullptr, "s", "lead", "hi", noop_token, nullptr, nullptr)
                  != ENTROPIC_OK);
        }
    }
}
