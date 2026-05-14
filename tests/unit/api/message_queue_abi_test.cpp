// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file message_queue_abi_test.cpp
 * @brief C ABI surface tests for the mid-gen message queue (gh#40).
 *
 * Exercises the public surface (`entropic_queue_user_message`,
 * `entropic_user_message_queue_depth`,
 * `entropic_clear_user_message_queue`, `entropic_set_queue_observer`)
 * for argument-validation and state-gating semantics that the
 * proposal's validation criteria pin down.
 *
 * The unit-test side of the queue covers happy-path drain semantics
 * by driving AgentEngine directly (mock inference). The C ABI surface
 * cannot drive a real agent loop without a configured engine, so this
 * file focuses on validation behavior — NULL handling, invalid-state
 * gating when no run is in flight, queue-full surfacing.
 *
 * @version 2.1.10
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>

// ── Argument validation ───────────────────────────────────────

SCENARIO("queue: NULL handle is rejected on every entry point",
         "[api][queue]") {
    REQUIRE(entropic_queue_user_message(nullptr, "x")
            == ENTROPIC_ERROR_INVALID_HANDLE);
    size_t depth = 999;
    REQUIRE(entropic_user_message_queue_depth(nullptr, &depth)
            == ENTROPIC_ERROR_INVALID_HANDLE);
    REQUIRE(entropic_clear_user_message_queue(nullptr)
            == ENTROPIC_ERROR_INVALID_HANDLE);
    REQUIRE(entropic_set_queue_observer(nullptr, nullptr, nullptr)
            == ENTROPIC_ERROR_INVALID_HANDLE);
}

SCENARIO("queue: NULL message argument is rejected",
         "[api][queue]") {
    entropic_handle_t h = nullptr;
    entropic_create(&h);
    REQUIRE(h != nullptr);
    REQUIRE(entropic_queue_user_message(h, nullptr)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
    entropic_destroy(h);
}

SCENARIO("queue: NULL out-pointer on depth is rejected",
         "[api][queue]") {
    entropic_handle_t h = nullptr;
    entropic_create(&h);
    REQUIRE(h != nullptr);
    REQUIRE(entropic_user_message_queue_depth(h, nullptr)
            == ENTROPIC_ERROR_INVALID_ARGUMENT);
    entropic_destroy(h);
}

// ── INVALID_STATE when no run in flight (validation criterion #1) ──

SCENARIO("queue: enqueue on idle handle returns INVALID_STATE",
         "[api][queue]") {
    entropic_handle_t h = nullptr;
    entropic_create(&h);
    REQUIRE(h != nullptr);
    // Engine not yet wired (no configure) → INVALID_STATE.
    REQUIRE(entropic_queue_user_message(h, "x")
            == ENTROPIC_ERROR_INVALID_STATE);
    entropic_destroy(h);
}

// ── Depth on unconfigured handle is zero, not an error ─────────

SCENARIO("queue: depth on unconfigured handle reads zero",
         "[api][queue]") {
    entropic_handle_t h = nullptr;
    entropic_create(&h);
    REQUIRE(h != nullptr);
    size_t depth = 42;
    REQUIRE(entropic_user_message_queue_depth(h, &depth)
            == ENTROPIC_OK);
    REQUIRE(depth == 0);
    entropic_destroy(h);
}

// ── Clear is a no-op on unconfigured handle, not an error ──────

SCENARIO("queue: clear on unconfigured handle is OK no-op",
         "[api][queue]") {
    entropic_handle_t h = nullptr;
    entropic_create(&h);
    REQUIRE(h != nullptr);
    REQUIRE(entropic_clear_user_message_queue(h) == ENTROPIC_OK);
    entropic_destroy(h);
}

// ── Observer can be registered before configure ────────────────

SCENARIO("queue: observer registration is allowed pre-configure",
         "[api][queue]") {
    entropic_handle_t h = nullptr;
    entropic_create(&h);
    REQUIRE(h != nullptr);
    auto cb = [](const char*, size_t, void*) {};
    REQUIRE(entropic_set_queue_observer(h, cb, nullptr)
            == ENTROPIC_OK);
    // Clear back to null also works.
    REQUIRE(entropic_set_queue_observer(h, nullptr, nullptr)
            == ENTROPIC_OK);
    entropic_destroy(h);
}

// ── ENTROPIC_ERROR_QUEUE_FULL error name maps cleanly ──────────

SCENARIO("queue: QUEUE_FULL error code has a stable name",
         "[api][queue]") {
    const char* name = entropic_error_name(ENTROPIC_ERROR_QUEUE_FULL);
    REQUIRE(name != nullptr);
    REQUIRE(std::string(name) == "ENTROPIC_ERROR_QUEUE_FULL");
}
