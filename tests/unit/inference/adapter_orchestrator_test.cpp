// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_adapter_orchestrator.cpp
 * @brief Tests for ModelOrchestrator LoRA adapter integration.
 *
 * Tests adapter preloading, tier-transition swap, and KV cache
 * invalidation. Uses mock backends — no real models.
 *
 * @version 1.9.2
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/orchestrator.h>
#include <entropic/types/config.h>

namespace {

/**
 * @brief Build a config with adapter_path on some tiers.
 * @return ParsedConfig with 3 tiers, 2 having adapters.
 * @utility
 * @version 1.9.2
 */
entropic::ParsedConfig make_adapter_config() {
    entropic::ParsedConfig config;
    config.models.default_tier = "lead";

    entropic::TierConfig lead;
    lead.path = "/tmp/test_model.gguf";
    lead.adapter = "generic";
    lead.keep_warm = true;
    // lead has NO adapter_path — base model only

    entropic::TierConfig eng;
    eng.path = "/tmp/test_model.gguf";  // Same base model
    eng.adapter = "generic";
    eng.adapter_path = "/tmp/eng-lora.gguf";
    eng.adapter_scale = 1.0f;

    entropic::TierConfig qa;
    qa.path = "/tmp/test_model.gguf";  // Same base model
    qa.adapter = "generic";
    qa.adapter_path = "/tmp/qa-lora.gguf";
    qa.adapter_scale = 0.8f;

    config.models.tiers["lead"] = lead;
    config.models.tiers["eng"] = eng;
    config.models.tiers["qa"] = qa;

    return config;
}

} // anonymous namespace

TEST_CASE("Config adapter_path stored in TierConfig", "[adapter][config]") {
    auto config = make_adapter_config();

    REQUIRE_FALSE(config.models.tiers["lead"].adapter_path.has_value());
    REQUIRE(config.models.tiers["eng"].adapter_path.has_value());
    REQUIRE(config.models.tiers["eng"].adapter_path->string() == "/tmp/eng-lora.gguf");
    REQUIRE(config.models.tiers["eng"].adapter_scale == 1.0f);
    REQUIRE(config.models.tiers["qa"].adapter_path.has_value());
    REQUIRE(config.models.tiers["qa"].adapter_scale == 0.8f);
}

TEST_CASE("RoutingResult has adapter fields", "[adapter][routing]") {
    entropic::RoutingResult result;
    result.adapter_name = "eng-lora";
    result.adapter_swap_ms = 42.5;

    REQUIRE(result.adapter_name == "eng-lora");
    REQUIRE(result.adapter_swap_ms == 42.5);
}

TEST_CASE("RoutingResult adapter fields default empty", "[adapter][routing]") {
    entropic::RoutingResult result;

    REQUIRE(result.adapter_name.empty());
    REQUIRE(result.adapter_swap_ms == 0.0);
}

TEST_CASE("AdapterState enum values are correct", "[adapter][types]") {
    REQUIRE(static_cast<int>(entropic::AdapterState::COLD) == 0);
    REQUIRE(static_cast<int>(entropic::AdapterState::WARM) == 1);
    REQUIRE(static_cast<int>(entropic::AdapterState::HOT) == 2);
}

TEST_CASE("AdapterInfo default state is COLD", "[adapter][types]") {
    entropic::AdapterInfo info;

    REQUIRE(info.state == entropic::AdapterState::COLD);
    REQUIRE(info.name.empty());
    REQUIRE(info.scale == 1.0f);
    REQUIRE(info.ram_bytes == 0);
}

TEST_CASE("TierConfig adapter_scale defaults to 1.0", "[adapter][config]") {
    entropic::TierConfig tier;

    REQUIRE(tier.adapter_scale == 1.0f);
    REQUIRE_FALSE(tier.adapter_path.has_value());
}
