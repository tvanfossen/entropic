// SPDX-License-Identifier: Apache-2.0
/**
 * @file secondary_model_loader_test.cpp
 * @brief Unit tests for SecondaryModelLoader role-state management.
 *
 * Exercises the loader without loading real models — calls
 * `ensure_loaded` with non-existent paths and asserts the failure
 * path keeps the slots map clean, then verifies the empty-loader
 * accessors (get/is_loaded/loaded_roles/release_role) behave
 * correctly. Real model loading is exercised by the model-test gate
 * (developer-run, GPU).
 *
 * @version 2.1.11
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/secondary_model_loader.h>

TEST_CASE("SecondaryModelLoader: empty loader has no roles",
          "[secondary_loader]") {
    entropic::SecondaryModelLoader loader;
    REQUIRE(loader.get("router") == nullptr);
    REQUIRE(loader.get("draft") == nullptr);
    REQUIRE_FALSE(loader.is_loaded("router"));
    REQUIRE(loader.loaded_roles().empty());
}

TEST_CASE("SecondaryModelLoader: release of unknown role is false",
          "[secondary_loader]") {
    entropic::SecondaryModelLoader loader;
    REQUIRE_FALSE(loader.release_role("router"));
    REQUIRE_FALSE(loader.release_role("draft"));
    REQUIRE_FALSE(loader.release_role("unknown_role"));
}

TEST_CASE("SecondaryModelLoader: ensure_loaded with bogus path fails cleanly",
          "[secondary_loader]") {
    entropic::SecondaryModelLoader loader;
    entropic::ModelConfig cfg;
    cfg.path = "/nonexistent/path/to/a/model.gguf";
    cfg.gpu_layers = 0;
    cfg.n_threads = 1;
    cfg.context_length = 512;

    REQUIRE_FALSE(loader.ensure_loaded("draft", cfg));

    // Failed load must NOT leave a stale slot — `get` returns
    // nullptr, `is_loaded` is false, `loaded_roles` is empty.
    REQUIRE(loader.get("draft") == nullptr);
    REQUIRE_FALSE(loader.is_loaded("draft"));
    REQUIRE(loader.loaded_roles().empty());
}

TEST_CASE("SecondaryModelLoader: shutdown is idempotent on empty",
          "[secondary_loader]") {
    entropic::SecondaryModelLoader loader;
    loader.shutdown();    // first call: no slots
    loader.shutdown();    // second call: still safe
    REQUIRE(loader.loaded_roles().empty());
}

TEST_CASE("SecondaryModelLoader: clear_all_prompt_caches no-op when empty",
          "[secondary_loader]") {
    entropic::SecondaryModelLoader loader;
    // Should not crash with no roles loaded.
    loader.clear_all_prompt_caches();
    REQUIRE(loader.loaded_roles().empty());
}

TEST_CASE("SecondaryModelLoader: get_shared returns empty for unknown role",
          "[secondary_loader]") {
    entropic::SecondaryModelLoader loader;
    auto sp = loader.get_shared("router");
    REQUIRE_FALSE(static_cast<bool>(sp));
}

// ── v2.3.10 [secondary_loader_topup] ──────────────────────
// Repeated failed loads, multi-role independence, shutdown safety.

TEST_CASE("SecondaryModelLoader v2.3.10 topup — failure isolation + idempotency",
          "[v2.3.10][inference][secondary_loader_topup]") {
    entropic::SecondaryModelLoader loader;
    entropic::ModelConfig cfg;
    cfg.gpu_layers = 0;
    cfg.n_threads = 1;
    cfg.context_length = 256;

    // Failed load twice on the same path — no stale-slot leak.
    cfg.path = "/no/such/repeat.gguf";
    REQUIRE_FALSE(loader.ensure_loaded("draft", cfg));
    REQUIRE_FALSE(loader.ensure_loaded("draft", cfg));
    REQUIRE(loader.get("draft") == nullptr);
    REQUIRE_FALSE(loader.is_loaded("draft"));
    REQUIRE_FALSE(static_cast<bool>(loader.get_shared("draft")));
    REQUIRE(loader.loaded_roles().empty());

    // Different roles all fail independently — no cross-contamination.
    cfg.path = "/no/such/router.gguf";
    REQUIRE_FALSE(loader.ensure_loaded("router", cfg));
    cfg.path = "/no/such/thinking.gguf";
    REQUIRE_FALSE(loader.ensure_loaded("thinking", cfg));
    REQUIRE(loader.get("router") == nullptr);
    REQUIRE(loader.get("thinking") == nullptr);
    REQUIRE_FALSE(loader.release_role("router"));
    REQUIRE_FALSE(loader.release_role("thinking"));

    // shutdown after failures + ensure_loaded still works after shutdown.
    loader.shutdown();
    REQUIRE(loader.loaded_roles().empty());
    cfg.path = "/no/such/after_shutdown.gguf";
    REQUIRE_FALSE(loader.ensure_loaded("draft", cfg));
}
