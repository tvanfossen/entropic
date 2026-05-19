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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
std::string make_gpu_config(const std::string& model_path,
                            const std::string& log_dir,
                            int gpu_layers) {
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "{"
        "\"log_level\":\"info\","
        "\"log_dir\":\"%s\","
        "\"ggml_logging\":true,"
        "\"models\":{"
            "\"default\":\"lead\","
            "\"lead\":{"
                "\"path\":\"%s\","
                "\"adapter\":\"qwen35\","
                "\"context_length\":8192,"
                "\"gpu_layers\":%d,"
                "\"cache_type_k\":\"q8_0\","
                "\"cache_type_v\":\"q8_0\","
                "\"flash_attn\":true"
            "}"
        "}"
        "}",
        log_dir.c_str(), model_path.c_str(), gpu_layers);
    return std::string(buf);
}
}  // namespace

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

// gh#58 v2.2.7 follow-up: real-GPU two-handle scenario. Tagged
// [.gpu][.realmodel] (Catch2 dot-prefix = excluded from default run).
// Requires GPU + a real GGUF. Override via ENTROPIC_TEST_GPU_MODEL
// env (default: ~/.entropic/models/Qwen3.5-0.8B-Q8_0.gguf, ~1 GB).
SCENARIO("Two GPU-resident handles can coexist (v2.2.8 target)",
         "[api][gh58][multi-handle][.gpu][.realmodel]") {
    const char* override_path = std::getenv("ENTROPIC_TEST_GPU_MODEL");
    std::string model_path;
    if (override_path) {
        model_path = override_path;
    } else {
        const char* home = std::getenv("HOME");
        model_path = std::string(home ? home : "/tmp")
                   + "/.entropic/models/Qwen3.5-0.8B-Q8_0.gguf";
    }

    // Allow a distinct second-model override to exercise the
    // consumer's qwen+gemma scenario.
    const char* override_path_b = std::getenv("ENTROPIC_TEST_GPU_MODEL_B");
    std::string model_path_b = override_path_b ? override_path_b : model_path;

    GIVEN("two handles configured with the same model on GPU") {
        auto cfg1 = make_gpu_config(model_path, "/tmp/entropic-gh58-h1", -1);
        auto cfg2 = make_gpu_config(model_path_b, "/tmp/entropic-gh58-h2", -1);

        entropic_handle_t h1 = nullptr;
        entropic_handle_t h2 = nullptr;
        REQUIRE(entropic_create(&h1) == ENTROPIC_OK);
        REQUIRE(entropic_create(&h2) == ENTROPIC_OK);

        WHEN("both are configured with gpu_layers=-1") {
            auto e1 = entropic_configure(h1, cfg1.c_str());
            auto e2 = entropic_configure(h2, cfg2.c_str());

            THEN("both succeed (no second-handle GPU activation failure)") {
                INFO("h1 last_error: "
                     << (entropic_last_error(h1) ?
                         entropic_last_error(h1) : "(null)"));
                INFO("h2 last_error: "
                     << (entropic_last_error(h2) ?
                         entropic_last_error(h2) : "(null)"));
                REQUIRE(e1 == ENTROPIC_OK);
                REQUIRE(e2 == ENTROPIC_OK);
            }

            AND_WHEN("each handle runs a short prompt") {
                if (e1 == ENTROPIC_OK && e2 == ENTROPIC_OK) {
                    char* r1 = nullptr;
                    char* r2 = nullptr;
                    auto run1 = entropic_run(h1, "hi", &r1);
                    auto run2 = entropic_run(h2, "hi", &r2);
                    THEN("both runs produce output without throwing") {
                        INFO("h1 run err=" << run1 << " last="
                             << (entropic_last_error(h1) ?
                                 entropic_last_error(h1) : ""));
                        INFO("h2 run err=" << run2 << " last="
                             << (entropic_last_error(h2) ?
                                 entropic_last_error(h2) : ""));
                        REQUIRE(run1 == ENTROPIC_OK);
                        REQUIRE(run2 == ENTROPIC_OK);
                    }
                    if (r1) { entropic_free(r1); }
                    if (r2) { entropic_free(r2); }
                }
            }
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
