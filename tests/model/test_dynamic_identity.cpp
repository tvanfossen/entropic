// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_dynamic_identity.cpp
 * @brief BDD subsystem test — runtime-created identity lifecycle (create,
 *        router-dirty flag, destroy).
 *
 * Validates dynamic identity creation, routing, and destruction.
 * Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test 8: Dynamic Identity + Routing ──────────────────────

SCENARIO("Runtime-created identity lifecycle (create, router-dirty, destroy)",
         "[model][test8]")
{
    GIVEN("a configured engine with static identities") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test8_dynamic_identity");
        IdentityManagerConfig mgr_cfg;
        mgr_cfg.allow_dynamic = true;
        IdentityManager mgr(mgr_cfg);

        // Create a static identity for baseline
        IdentityConfig lead;
        lead.name = "lead";
        lead.system_prompt = "You are the lead engineer.";
        lead.focus = {"management", "coordination"};
        lead.origin = IdentityOrigin::STATIC;
        mgr.load_static({lead});
        REQUIRE(mgr.count() >= 1);
        mgr.clear_router_dirty();

        WHEN("I create a dynamic identity npc_guard") {
            IdentityConfig npc;
            npc.name = "npc-guard";
            npc.system_prompt = "You are a palace guard.";
            npc.focus = {"security", "guard duties"};
            npc.routable = true;
            auto err = mgr.create(npc);
            REQUIRE(err == ENTROPIC_OK);

            THEN("identity exists, marks the router dirty, and destroy "
                 "cleans up") {
                REQUIRE(mgr.has("npc-guard"));
                // audit task #71: the prior version also called
                // g_ctx.orchestrator->route() and asserted tier non-empty, but
                // g_ctx has routing.enabled=false so route() short-circuits to
                // the default tier — a vacuous "participates in routing" claim
                // (list_routable() has no production consumer). Dropped. The
                // router-dirty flag below IS a real signal that the routable
                // identity set changed.
                REQUIRE(mgr.is_router_dirty());
                REQUIRE(mgr.count_dynamic() == 1);

                mgr.clear_router_dirty();
                auto err2 = mgr.destroy("npc-guard");
                REQUIRE(err2 == ENTROPIC_OK);
                REQUIRE_FALSE(mgr.has("npc-guard"));
                REQUIRE(mgr.is_router_dirty());
                REQUIRE(mgr.count_dynamic() == 0);
                end_test_log();
            }
        }
    }
}
