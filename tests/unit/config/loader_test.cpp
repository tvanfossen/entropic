// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_config_loader.cpp
 * @brief BDD tests for config loading and layered merge.
 * @version 1.8.1
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <entropic/config/loader.h>
#include <entropic/config/bundled_models.h>
#include <cstdlib>
#include <filesystem>

/**
 * @brief Return the path to the test data directory.
 * @return Filesystem path defined by TEST_DATA_DIR compile definition.
 * @version 1.8.1
 * @internal
 */
static std::filesystem::path test_data()
{
    return std::filesystem::path(TEST_DATA_DIR);
}

/**
 * @brief Load the test bundled models registry from test data.
 * @return Populated BundledModels registry.
 * @version 1.8.1
 * @internal
 */
static entropic::config::BundledModels load_test_registry()
{
    entropic::config::BundledModels registry;
    auto err = registry.load(test_data() / "bundled_models.yaml");
    REQUIRE(err.empty());
    return registry;
}

SCENARIO("Parse minimal config", "[config][loader]") {
    GIVEN("YAML with only log_level") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;

        WHEN("parse_config_file is called") {
            auto err = entropic::config::parse_config_file(
                test_data() / "minimal_config.yaml", registry, config);

            THEN("it succeeds") {
                REQUIRE(err.empty());
            }

            THEN("log_level is overridden") {
                REQUIRE(config.log_level == "DEBUG");
            }

            THEN("all other fields have defaults") {
                REQUIRE(config.models.default_tier == "lead");
                REQUIRE(config.routing.enabled == false);
                REQUIRE(config.compaction.threshold_percent
                        == Catch::Approx(0.75f));
            }
        }
    }
}

SCENARIO("Parse config with tiers", "[config][loader]") {
    GIVEN("Config with lead and eng tiers") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;

        WHEN("parse_config_file is called") {
            auto err = entropic::config::parse_config_file(
                test_data() / "test_config.yaml", registry, config);

            THEN("it succeeds") {
                REQUIRE(err.empty());
            }

            THEN("tiers are parsed") {
                REQUIRE(config.models.tiers.size() == 2);
                REQUIRE(config.models.tiers.count("lead") == 1);
                REQUIRE(config.models.tiers.count("eng") == 1);
            }

            THEN("tier fields are correct") {
                auto& lead = config.models.tiers["lead"];
                REQUIRE(lead.adapter == "qwen35");
                REQUIRE(lead.context_length == 131072);
                REQUIRE(lead.gpu_layers == 40);
            }

            THEN("model paths are resolved from registry") {
                // v2.0.5: resolve() falls back to ~/.entropic/models/<name>.gguf
                // when ENTROPIC_MODEL_DIR is unset and no candidate exists on
                // disk. The previous hardcoded ~/models/gguf/ path is gone.
                auto& lead = config.models.tiers["lead"];
                auto home = std::filesystem::path(std::getenv("HOME"));
                auto expected = home / ".entropic" / "models"
                                / "Qwen3.5-35B-A3B-UD-IQ3_XXS.gguf";
                REQUIRE(lead.path == expected);
            }

            THEN("MCP config is parsed") {
                REQUIRE(config.mcp.enable_filesystem == true);
                REQUIRE(config.mcp.filesystem.allow_outside_root == true);
            }
        }
    }
}

SCENARIO("Layered merge — project overlays global", "[config][loader]") {
    GIVEN("Global config with lead.gpu_layers=40") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;

        // Load global config first
        auto err = entropic::config::parse_config_file(
            test_data() / "test_config.yaml", registry, config);
        REQUIRE(err.empty());

        WHEN("project config overrides only gpu_layers") {
            err = entropic::config::parse_config_file(
                test_data() / "overlay_config.yaml", registry, config);

            THEN("it succeeds") {
                REQUIRE(err.empty());
            }

            THEN("gpu_layers is overridden") {
                REQUIRE(config.models.tiers["lead"].gpu_layers == 20);
            }

            THEN("adapter is preserved from global") {
                REQUIRE(config.models.tiers["lead"].adapter == "qwen35");
            }

            THEN("eng tier is preserved from global") {
                REQUIRE(config.models.tiers.count("eng") == 1);
            }
        }
    }
}
