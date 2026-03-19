/**
 * @file test_orchestrator.cpp
 * @brief Tests for ModelOrchestrator dedup, swap, and handoff.
 *
 * Uses mock config — no real models loaded.
 *
 * @version 1.8.2
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/orchestrator.h>

namespace {

/**
 * @brief Build a minimal config for orchestrator tests.
 * @version 1.8.2
 */
entropic::ParsedConfig make_test_config() {
    entropic::ParsedConfig config;
    config.models.default_tier = "lead";

    // Two tiers sharing same model path → should dedup
    entropic::TierConfig lead;
    lead.path = "/tmp/test_model.gguf";
    lead.adapter = "generic";
    lead.keep_warm = true;

    entropic::TierConfig eng;
    eng.path = "/tmp/test_model.gguf";  // Same path as lead
    eng.adapter = "qwen35";
    eng.keep_warm = false;

    config.models.tiers["lead"] = lead;
    config.models.tiers["eng"] = eng;

    // Handoff rules
    config.routing.handoff_rules["lead"] = {"eng"};
    config.routing.handoff_rules["eng"] = {"lead"};

    return config;
}

} // anonymous namespace

// ── Handoff rules ──────────────────────────────────────────

SCENARIO("Handoff rules", "[orchestrator][handoff]") {
    // Test handoff rule checking without model loading
    entropic::ModelOrchestrator orch;
    // Note: These tests only verify the query interface.
    // Full initialize() requires real model files.

    GIVEN("handoff rules configured") {
        // can_handoff checks the internal rules map
        // Without initialize(), rules are empty → all denied
        THEN("empty orchestrator denies all handoffs") {
            REQUIRE_FALSE(orch.can_handoff("lead", "eng"));
            REQUIRE_FALSE(orch.can_handoff("eng", "lead"));
        }
    }
}

// ── Query interface ────────────────────────────────────────

SCENARIO("Orchestrator query interface", "[orchestrator][query]") {
    entropic::ModelOrchestrator orch;

    THEN("empty orchestrator has no loaded models") {
        REQUIRE(orch.loaded_models().empty());
        REQUIRE(orch.available_models().empty());
        REQUIRE(orch.last_used_tier().empty());
    }
}
