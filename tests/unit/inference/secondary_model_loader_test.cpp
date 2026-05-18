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
