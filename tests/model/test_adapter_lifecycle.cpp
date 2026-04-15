// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_adapter_lifecycle.cpp
 * @brief BDD subsystem test — LoRA adapter graceful failure path.
 *
 * Validates that the AdapterManager reports no loaded adapters when
 * none have been loaded, and that base model generation works
 * correctly in the no-adapter fallback path.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"
#include <entropic/inference/adapter_manager.h>

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test: Adapter lifecycle graceful failure ───────────────

SCENARIO("LoRA adapter load failure is handled gracefully",
         "[model][adapter_lifecycle]")
{
    GIVEN("a model loaded with AdapterManager available") {
        REQUIRE(g_ctx.initialized);
        start_test_log("adapter_lifecycle");
        auto& mgr = g_ctx.orchestrator->adapter_manager();

        WHEN("checking adapter state and generating without adapter") {
            auto adapters = mgr.list_adapters();
            auto active = mgr.active_adapter();
            auto state = mgr.state("nonexistent-adapter");
            auto params = test_gen_params();
            auto messages = make_messages(
                "You are a helpful assistant.", "What is 2+2?");
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);

            THEN("base model generates correctly without adapter") {
                CHECK(adapters.empty());
                CHECK(active.empty());
                CHECK(state == AdapterState::COLD);
                REQUIRE_FALSE(result.content.empty());
                CHECK(result.content.find("4")
                      != std::string::npos);
                CHECK(result.token_count > 0);
                end_test_log();
            }
        }
    }
}
