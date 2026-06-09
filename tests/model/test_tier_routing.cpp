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

// ── Test 3b (v2.8.1, review #4): cleared-prompt RED control ──────────
// Exercises the FALSE branch of classify_task's prompt gate. With NO
// classification_prompt the router gets the bare "<msg> ->", and a general
// instruct model just CONTINUES the text (it is never told the digit scheme),
// so no routing digit is emitted → model_raw empty → silent fallback to the
// default tier. This is exactly the no-op the v2.8.0 fix closed; asserting it
// here makes the GREEN scenario above non-vacuous (the bare path is the thing
// that USED to "pass" while routing did nothing).
SCENARIO("Cleared classification_prompt makes routing a no-op (RED control)",
         "[model][test3b]")
{
    GIVEN("the routing config with classification_prompt cleared") {
        start_test_log("test3b_cleared_prompt");
        config::BundledModels reg;
        REQUIRE(load_registry(reg));
        ParsedConfig cfg;
        REQUIRE(load_test_config(reg, cfg, routing_config_path()));
        cfg.routing.classification_prompt.reset();  // force the bare "<msg> ->"
        ModelOrchestrator orch;
        REQUIRE(orch.initialize(cfg));

        WHEN("a programming task is routed on the bare path") {
            auto code = make_messages(
                "", "Write a Python function that sorts a list");
            auto tier = orch.route(code);
            auto rr = orch.last_routing_result();

            THEN("the router emits no routing digit (no-op fallback)") {
                INFO("routed tier=[" << tier << "] model_raw=["
                     << rr.model_raw << "]");
                // Bare "<msg> ->" → general model continues prose, no tier digit
                // → model_raw empty. CHECK (not REQUIRE): a 1-token greedy on a
                // code prompt won't emit a bare tier digit, but GPU decode is
                // non-deterministic — record a stray hit rather than hard-fail.
                CHECK(rr.model_raw.empty());
                end_test_log();
            }
        }
    }
}
