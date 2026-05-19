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

SCENARIO("entropic_last_error returns per-handle state (v2.2.6)",
         "[api][gh58][multi-handle][last_error]") {
    GIVEN("two configured handles") {
        entropic_handle_t h1 = nullptr;
        entropic_handle_t h2 = nullptr;
        REQUIRE(entropic_create(&h1) == ENTROPIC_OK);
        REQUIRE(entropic_create(&h2) == ENTROPIC_OK);
        REQUIRE(entropic_configure(h1, R"({"log_level":"WARN"})") == ENTROPIC_OK);
        REQUIRE(entropic_configure(h2, R"({"log_level":"WARN"})") == ENTROPIC_OK);

        WHEN("re-configure is attempted on h1 (sets last_error)") {
            auto err = entropic_configure(h1, R"({"log_level":"WARN"})");
            REQUIRE(err == ENTROPIC_ERROR_INVALID_STATE);

            THEN("entropic_last_error(h1) returns the message") {
                const char* msg = entropic_last_error(h1);
                REQUIRE(msg != nullptr);
                // Pre-v2.2.6 this returned a thread-local global that
                // ignored the handle entirely.
                REQUIRE(std::string(msg) == "handle already configured");
            }

            THEN("entropic_last_error(h2) is independent of h1") {
                const char* msg2 = entropic_last_error(h2);
                REQUIRE(msg2 != nullptr);
                REQUIRE(std::string(msg2) != "handle already configured");
            }
        }

        entropic_destroy(h1);
        entropic_destroy(h2);
    }
}

SCENARIO("h1.run() after h2.configure() does not segfault (v2.2.6)",
         "[api][gh58][multi-handle][run]") {
    // Direct repro of the consumer's gh#58 follow-up symptom. Pre-
    // v2.2.6 the second configure deleted the first handle's
    // InterfaceContext (process-global static `s_ctx` in
    // src/inference/interface_factory.cpp), leaving h1's
    // inference_iface.user_data dangling. h1.run() then segfaulted in
    // ModelOrchestrator::route via the iface_route callback.
    GIVEN("two handles configured without models") {
        entropic_handle_t h1 = nullptr;
        entropic_handle_t h2 = nullptr;
        REQUIRE(entropic_create(&h1) == ENTROPIC_OK);
        REQUIRE(entropic_create(&h2) == ENTROPIC_OK);
        REQUIRE(entropic_configure(h1, R"({"log_level":"WARN"})") == ENTROPIC_OK);
        REQUIRE(entropic_configure(h2, R"({"log_level":"WARN"})") == ENTROPIC_OK);

        WHEN("h1.run() is called after h2.configure()") {
            char* result = nullptr;
            auto err = entropic_run(h1, "Say hi.", &result);

            THEN("the call returns (no segfault) and last_error is readable") {
                // The actual return code with no-model config is a
                // separate engine concern (today: OK with 0-char
                // output; arguably should be a no-model error).
                // What gh#58 needs is: no crash, and last_error is
                // readable per-handle, regardless of value.
                (void)err;
                const char* msg = entropic_last_error(h1);
                REQUIRE(msg != nullptr);
            }

            if (result) { entropic_free(result); }
        }

        entropic_destroy(h1);
        entropic_destroy(h2);
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
