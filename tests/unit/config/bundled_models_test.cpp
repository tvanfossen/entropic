/**
 * @file test_bundled_models.cpp
 * @brief BDD tests for BundledModels registry.
 * @version 1.8.1
 */

#include <catch2/catch_test_macros.hpp>
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

SCENARIO("BundledModels loads registry from YAML", "[config][bundled_models]") {
    GIVEN("The test bundled_models.yaml file") {
        entropic::config::BundledModels registry;

        WHEN("registry.load() is called") {
            auto err = registry.load(test_data() / "bundled_models.yaml");

            THEN("it succeeds") {
                REQUIRE(err.empty());
            }

            THEN("registry contains expected entries") {
                REQUIRE(registry.contains("primary"));
                REQUIRE(registry.contains("router"));
                REQUIRE_FALSE(registry.contains("nonexistent"));
            }

            THEN("primary entry has correct fields") {
                auto* entry = registry.get("primary");
                REQUIRE(entry != nullptr);
                REQUIRE(entry->name == "Qwen3.5-35B-A3B-UD-IQ3_XXS");
                REQUIRE(entry->adapter == "qwen35");
                REQUIRE(entry->size_gb == 13.1);
            }

            THEN("router entry has correct size") {
                auto* entry = registry.get("router");
                REQUIRE(entry != nullptr);
                REQUIRE(entry->size_gb == 0.6);
            }
        }
    }
}

SCENARIO("BundledModels resolves keys to paths", "[config][bundled_models]") {
    GIVEN("A loaded registry") {
        entropic::config::BundledModels registry;
        auto err = registry.load(test_data() / "bundled_models.yaml");
        REQUIRE(err.empty());

        // v2.0.5: resolve() uses ENTROPIC_MODEL_DIR → ~/.entropic/models →
        // /opt/entropic/models discovery. Unset the env var so the test
        // exercises the home-dir fallback deterministically.
        unsetenv("ENTROPIC_MODEL_DIR");

        WHEN("resolving a registry key with no env override") {
            auto path = registry.resolve("primary");

            THEN("it returns ~/.entropic/models/{name}.gguf fallback") {
                auto home = std::filesystem::path(std::getenv("HOME"));
                auto expected = home / ".entropic" / "models"
                                / "Qwen3.5-35B-A3B-UD-IQ3_XXS.gguf";
                REQUIRE(path == expected);
            }
        }

        WHEN("ENTROPIC_MODEL_DIR points at an arbitrary directory") {
            setenv("ENTROPIC_MODEL_DIR", "/custom/models", 1);
            auto path = registry.resolve("primary");
            unsetenv("ENTROPIC_MODEL_DIR");

            THEN("resolve uses that directory") {
                REQUIRE(path == std::filesystem::path("/custom/models")
                                / "Qwen3.5-35B-A3B-UD-IQ3_XXS.gguf");
            }
        }

        WHEN("resolving an absolute path") {
            auto path = registry.resolve("/opt/models/custom.gguf");

            THEN("it returns the path unchanged") {
                REQUIRE(path == "/opt/models/custom.gguf");
            }
        }

        WHEN("resolving a tilde path") {
            auto path = registry.resolve("~/models/custom.gguf");

            THEN("it expands the tilde") {
                auto home = std::filesystem::path(std::getenv("HOME"));
                REQUIRE(path == home / "models" / "custom.gguf");
            }
        }
    }
}
