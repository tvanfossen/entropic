// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file multi_handle_test.cpp
 * @brief gh#58 — two entropic_handle_t in one process.
 *
 * Mirrors the sassafras-class consumer probe at the C-API level.
 * Validates that creating, configuring (via JSON — no real models),
 * and destroying multiple handles concurrently in one process does
 * not throw or corrupt either handle. Full ensemble-with-real-models
 * coverage requires a GPU and lives in the consumer's test suite.
 *
 * @version 2.2.5
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>

#include <cstring>

SCENARIO("Two handles configure independently", "[api][gh58][multi-handle]") {
    GIVEN("two created handles in the same process") {
        entropic_handle_t h1 = nullptr;
        entropic_handle_t h2 = nullptr;
        REQUIRE(entropic_create(&h1) == ENTROPIC_OK);
        REQUIRE(entropic_create(&h2) == ENTROPIC_OK);
        REQUIRE(h1 != nullptr);
        REQUIRE(h2 != nullptr);
        REQUIRE(h1 != h2);

        WHEN("both are configured with minimal JSON") {
            auto e1 = entropic_configure(h1, R"({"log_level":"WARN"})");
            auto e2 = entropic_configure(h2, R"({"log_level":"WARN"})");

            THEN("both succeed (no throw, no global-state cross-talk)") {
                REQUIRE(e1 == ENTROPIC_OK);
                REQUIRE(e2 == ENTROPIC_OK);
            }
        }

        entropic_destroy(h1);
        entropic_destroy(h2);
    }
}

SCENARIO("Double-configure on one handle is refused",
         "[api][gh58][multi-handle]") {
    GIVEN("a handle already configured") {
        entropic_handle_t h = nullptr;
        REQUIRE(entropic_create(&h) == ENTROPIC_OK);
        REQUIRE(entropic_configure(h, R"({"log_level":"WARN"})") == ENTROPIC_OK);

        WHEN("configure is called again on the same handle") {
            auto err = entropic_configure(h, R"({"log_level":"WARN"})");

            THEN("INVALID_STATE is returned, not silent re-init") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_STATE);
            }
        }

        entropic_destroy(h);
    }
}

SCENARIO("Three handles can coexist", "[api][gh58][multi-handle]") {
    GIVEN("three created handles") {
        entropic_handle_t hs[3] = {nullptr, nullptr, nullptr};
        for (auto& h : hs) { REQUIRE(entropic_create(&h) == ENTROPIC_OK); }

        WHEN("each is configured independently") {
            entropic_error_t errs[3];
            for (int i = 0; i < 3; ++i) {
                errs[i] = entropic_configure(
                    hs[i], R"({"log_level":"WARN"})");
            }

            THEN("all three succeed — matches qwen+gemma+nemotron triple") {
                REQUIRE(errs[0] == ENTROPIC_OK);
                REQUIRE(errs[1] == ENTROPIC_OK);
                REQUIRE(errs[2] == ENTROPIC_OK);
            }
        }

        for (auto h : hs) { entropic_destroy(h); }
    }
}
