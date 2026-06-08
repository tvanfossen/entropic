// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_tier_routing.cpp
 * @brief BDD subsystem test — classification routes to correct identity.
 *
 * Validates that the router classifies prompts to appropriate tiers.
 * Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

#include <set>

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test 3: Tier Routing (audit task #71) ───────────────────────────
// PRIOR vacuous form: routed on the shared g_ctx whose config.local.yaml sets
// routing.enabled=false, so route() short-circuited to the default tier and
// REQUIRE_FALSE(tier.empty()) passed without classify_task ever running.
// This builds a LOCAL routing-ENABLED orchestrator from routing_config.yaml
// (which sets routing.classification_prompt so the router is actually told the
// digit scheme — see classify_task, audit task #71) and asserts model_raw (the
// matched tier_map digit) is non-empty, which can ONLY be true if classify_task
// ran the router AND it emitted a routing digit.

SCENARIO("Classification runs the router and yields a configured tier",
         "[model][test3]")
{
    GIVEN("a routing-ENABLED orchestrator (router model + classify prompt)") {
        start_test_log("test3_tier_routing");
        config::BundledModels reg;
        REQUIRE(load_registry(reg));
        ParsedConfig cfg;
        REQUIRE(load_test_config(reg, cfg, routing_config_path()));
        ModelOrchestrator orch;
        REQUIRE(orch.initialize(cfg));
        const std::set<std::string> kConfigured = {"eng", "qa"};

        WHEN("a programming task is routed") {
            auto code = make_messages(
                "", "Write a Python function that sorts a list");
            auto tier = orch.route(code);
            auto rr = orch.last_routing_result();

            THEN("the router actually classified (raw digit present)") {
                INFO("routed tier=[" << tier << "] model_raw=["
                     << rr.model_raw << "]");
                // model_raw is the matched tier_map digit; "" when routing is
                // disabled OR the router emitted no digit — the bug this catches.
                REQUIRE_FALSE(rr.model_raw.empty());
                REQUIRE(kConfigured.count(tier) == 1);
                end_test_log();
            }
        }
    }
}
