// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_config_edge_cases.cpp
 * @brief Edge case tests for config validation and loading.
 *
 * Validates that config validation rejects invalid inputs with clear
 * error messages, and that config loading handles malformed files
 * gracefully (never crashes, always returns structured errors).
 *
 * @version 1.9.14
 */

#include <entropic/config/validate.h>
#include <entropic/config/loader.h>
#include <entropic/config/bundled_models.h>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace entropic;
using namespace entropic::config;

// ── Helpers ─────────────────────────────────────────────

/**
 * @brief RAII temp file that deletes on destruction.
 * @internal
 * @version 1.9.14
 */
class TempConfigFile {
public:
    /**
     * @brief Create a temp file with given content.
     * @param filename Name within temp directory.
     * @param content File content.
     * @version 1.9.14
     */
    TempConfigFile(const std::string& filename,
                   const std::string& content)
        : path_(std::filesystem::temp_directory_path() / filename) {
        std::ofstream f(path_);
        f << content;
    }

    ~TempConfigFile() {
        std::filesystem::remove(path_);
    }

    /**
     * @brief Get the file path.
     * @return Filesystem path.
     * @version 1.9.14
     */
    const std::filesystem::path& path() const { return path_; }

    TempConfigFile(const TempConfigFile&) = delete;
    TempConfigFile& operator=(const TempConfigFile&) = delete;

private:
    std::filesystem::path path_;
};

// ── Validation edge cases ───────────────────────────────

SCENARIO("Negative context_length rejected", "[edge][config]") {
    GIVEN("A ModelConfig with negative context_length") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.context_length = -500;

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails with clear error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }
}

SCENARIO("Zero context_length rejected", "[edge][config]") {
    GIVEN("A ModelConfig with zero context_length") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.context_length = 0;

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }
}

SCENARIO("INT_MAX context_length rejected", "[edge][config]") {
    GIVEN("A ModelConfig with INT_MAX context_length") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.context_length = 2147483647;

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails — exceeds 131072 maximum") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }
}

SCENARIO("Empty adapter rejected", "[edge][config]") {
    GIVEN("A ModelConfig with empty adapter string") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.adapter = "";

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
            }
        }
    }
}

SCENARIO("Malformed allowed_tools rejected", "[edge][config]") {
    GIVEN("allowed_tools with no dot separator") {
        std::vector<std::string> tools = {"nodot"};

        WHEN("validate_allowed_tools is called") {
            auto err = validate_allowed_tools(tools);

            THEN("it fails — requires server.tool format") {
                REQUIRE_FALSE(err.empty());
            }
        }
    }
}

SCENARIO("Fallback tier referencing unknown tier", "[edge][config]") {
    GIVEN("A fallback tier name not in tiers map") {
        std::unordered_map<std::string, TierConfig> tiers;
        TierConfig tc;
        tc.path = "/tmp/model.gguf";
        tiers["lead"] = tc;

        WHEN("validating fallback to nonexistent tier") {
            auto err = validate_fallback_tier("unknown_tier", tiers);

            THEN("it fails with descriptive error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("unknown_tier") != std::string::npos);
            }
        }
    }
}

SCENARIO("Tier map referencing nonexistent tier", "[edge][config]") {
    GIVEN("A tier_map pointing to undefined tier") {
        std::unordered_map<std::string, TierConfig> tiers;
        TierConfig tc;
        tc.path = "/tmp/model.gguf";
        tiers["lead"] = tc;

        std::unordered_map<std::string, std::string> tier_map;
        tier_map["chat"] = "ghost_tier";

        WHEN("validate_tier_map is called") {
            auto err = validate_tier_map(tier_map, tiers);

            THEN("it fails — ghost_tier does not exist") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("ghost_tier") != std::string::npos);
            }
        }
    }
}

SCENARIO("Handoff rules referencing nonexistent tier",
         "[edge][config]") {
    GIVEN("handoff_rules with unknown target tier") {
        std::unordered_map<std::string, TierConfig> tiers;
        TierConfig tc;
        tc.path = "/tmp/model.gguf";
        tiers["lead"] = tc;

        std::unordered_map<std::string, std::vector<std::string>> rules;
        rules["lead"] = {"phantom"};

        WHEN("validate_handoff_rules is called") {
            auto err = validate_handoff_rules(rules, tiers);

            THEN("it fails — phantom tier not defined") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("phantom") != std::string::npos);
            }
        }
    }
}

// ── File loading edge cases ─────────────────────────────

SCENARIO("Empty config file returns descriptive error",
         "[edge][config]") {
    GIVEN("A zero-byte config file") {
        TempConfigFile tmp("entropic_edge_empty.yaml", "");
        BundledModels registry;
        ParsedConfig config;

        WHEN("parse_config_file is called") {
            auto err = parse_config_file(tmp.path(), registry, config);

            THEN("returns error mentioning the file, no crash") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("config") != std::string::npos);
            }
        }
    }
}

SCENARIO("Non-map YAML root returns error", "[edge][config]") {
    GIVEN("A config file with a YAML array root instead of map") {
        TempConfigFile tmp("entropic_edge_array.yaml",
                           "- item1\n- item2\n- item3\n");
        BundledModels registry;
        ParsedConfig config;

        WHEN("parse_config_file is called") {
            auto err = parse_config_file(tmp.path(), registry, config);

            THEN("it returns a root-type error, no crash") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("root") != std::string::npos);
            }
        }
    }
}

SCENARIO("Scalar YAML root returns error", "[edge][config]") {
    GIVEN("A config file with a plain string as root") {
        TempConfigFile tmp("entropic_edge_scalar.yaml",
                           "just a string\n");
        BundledModels registry;
        ParsedConfig config;

        WHEN("parse_config_file is called") {
            auto err = parse_config_file(tmp.path(), registry, config);

            THEN("it returns an error, no crash") {
                REQUIRE_FALSE(err.empty());
            }
        }
    }
}

SCENARIO("Nonexistent config file returns error", "[edge][config]") {
    GIVEN("A path to a file that does not exist") {
        BundledModels registry;
        ParsedConfig config;

        WHEN("parse_config_file is called") {
            auto err = parse_config_file(
                "/tmp/entropic_edge_no_such_file_97531.yaml",
                registry, config);

            THEN("it returns a file I/O error") {
                REQUIRE_FALSE(err.empty());
            }
        }
    }
}
