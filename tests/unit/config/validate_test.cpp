// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_config_validate.cpp
 * @brief BDD tests for config validation functions.
 * @version 1.8.1
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/config/validate.h>

using namespace entropic;
using namespace entropic::config;

SCENARIO("ModelConfig validation", "[config][validate]") {
    GIVEN("A valid ModelConfig") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.context_length = 16384;

        WHEN("validate is called") {
            THEN("it passes") {
                REQUIRE(validate(cfg).empty());
            }
        }
    }

    GIVEN("ModelConfig with context_length below minimum") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.context_length = 256;

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails with range error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }

    GIVEN("ModelConfig with context_length above maximum") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.context_length = 200000;

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails with range error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }

    GIVEN("ModelConfig with empty adapter") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.adapter = "";

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("adapter") != std::string::npos);
            }
        }
    }
}

SCENARIO("Allowed tools validation", "[config][validate]") {
    GIVEN("Tools with proper server.tool format") {
        std::vector<std::string> tools = {"filesystem.read_file",
                                          "git.status"};

        WHEN("validate_allowed_tools is called") {
            THEN("it passes") {
                REQUIRE(validate_allowed_tools(tools).empty());
            }
        }
    }

    GIVEN("Tools without dot separator") {
        std::vector<std::string> tools = {"bare_name"};

        WHEN("validate_allowed_tools is called") {
            auto err = validate_allowed_tools(tools);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("bare_name") != std::string::npos);
            }
        }
    }
}

SCENARIO("ModelsConfig validation", "[config][validate]") {
    GIVEN("Default tier not in tiers") {
        ModelsConfig cfg;
        TierConfig tier;
        tier.path = "/tmp/model.gguf";
        cfg.tiers["lead"] = tier;
        cfg.default_tier = "nonexistent";

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails with clear error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("nonexistent") != std::string::npos);
                REQUIRE(err.find("not in tiers") != std::string::npos);
            }
        }
    }

    GIVEN("Default tier exists in tiers") {
        ModelsConfig cfg;
        TierConfig tier;
        tier.path = "/tmp/model.gguf";
        cfg.tiers["lead"] = tier;
        cfg.default_tier = "lead";

        WHEN("validate is called") {
            THEN("it passes") {
                REQUIRE(validate(cfg).empty());
            }
        }
    }
}

SCENARIO("RoutingConfig validation", "[config][validate]") {
    ModelsConfig models;
    TierConfig tier;
    tier.path = "/tmp/model.gguf";
    models.tiers["lead"] = tier;
    models.tiers["eng"] = tier;
    models.default_tier = "lead";

    GIVEN("routing enabled without router model") {
        RoutingConfig routing;
        routing.enabled = true;
        routing.fallback_tier = "lead";

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("router is not configured")
                        != std::string::npos);
            }
        }
    }

    GIVEN("fallback tier not in tiers") {
        RoutingConfig routing;
        routing.fallback_tier = "nonexistent";

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("nonexistent") != std::string::npos);
            }
        }
    }

    GIVEN("tier_map references undefined tier") {
        RoutingConfig routing;
        routing.fallback_tier = "lead";
        routing.tier_map["code"] = "nonexistent";

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("nonexistent") != std::string::npos);
            }
        }
    }

    GIVEN("routing enabled + classification_prompt set but tier_map empty") {
        // v2.8.1 (review #4): an empty tier_map passes validate_tier_map (it
        // only checks map VALUES), so a configured classification_prompt with
        // no tier_map silently re-creates the v2.8.0 no-op. Router must be
        // present so validation reaches the new cross-field check.
        RoutingConfig routing;
        routing.enabled = true;
        routing.fallback_tier = "lead";
        routing.classification_prompt = "Classify: 1=eng 2=qa\n";
        // tier_map intentionally left empty
        models.router = ModelConfig{};
        models.router->path = "/tmp/router.gguf";

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails — the prompt is inert without a tier_map") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("tier_map is empty") != std::string::npos);
            }
        }
    }

    GIVEN("handoff_rules source not in tiers") {
        RoutingConfig routing;
        routing.fallback_tier = "lead";
        routing.handoff_rules["missing"] = {"lead"};

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("missing") != std::string::npos);
            }
        }
    }

    GIVEN("handoff_rules target not in tiers") {
        RoutingConfig routing;
        routing.fallback_tier = "lead";
        routing.handoff_rules["lead"] = {"missing"};

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("missing") != std::string::npos);
            }
        }
    }
}

SCENARIO("CompactionConfig validation", "[config][validate]") {
    GIVEN("Warning threshold >= compaction threshold") {
        CompactionConfig cfg;
        cfg.threshold_percent = 0.75f;
        cfg.warning_threshold_percent = 0.8f;

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("warning_threshold_percent")
                        != std::string::npos);
            }
        }
    }

    GIVEN("Valid compaction config") {
        CompactionConfig cfg;
        cfg.threshold_percent = 0.75f;
        cfg.warning_threshold_percent = 0.6f;

        WHEN("validate is called") {
            THEN("it passes") {
                REQUIRE(validate(cfg).empty());
            }
        }
    }

    // ── v2.3.10: cover remaining failure branches ──

    GIVEN("threshold_percent below the 0.5 floor") {
        CompactionConfig cfg;
        cfg.threshold_percent = 0.3f;
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a threshold_percent error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("threshold_percent") != std::string::npos);
            }
        }
    }

    GIVEN("preserve_recent_turns above the cap") {
        CompactionConfig cfg;
        cfg.threshold_percent = 0.75f;
        cfg.warning_threshold_percent = 0.6f;
        cfg.preserve_recent_turns = 99;
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a preserve_recent_turns error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("preserve_recent_turns")
                        != std::string::npos);
            }
        }
    }

    GIVEN("tool_result_ttl below 1") {
        CompactionConfig cfg;
        cfg.threshold_percent = 0.75f;
        cfg.warning_threshold_percent = 0.6f;
        cfg.preserve_recent_turns = 3;
        cfg.tool_result_ttl = 0;
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a tool_result_ttl error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("tool_result_ttl") != std::string::npos);
            }
        }
    }
}

// ── v2.3.10: cover remaining ModelConfig + ModelsConfig branches ──

SCENARIO("ModelConfig validation — failure modes",
         "[config][validate][v2.3.10][failure-mode]") {
    GIVEN("context_length below the 512 floor") {
        ModelConfig cfg;
        cfg.context_length = 256;
        cfg.adapter = "qwen35";
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a context_length error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }

    GIVEN("context_length above the ceiling") {
        ModelConfig cfg;
        cfg.context_length = 999999;
        cfg.adapter = "qwen35";
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a context_length error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }

    GIVEN("empty adapter") {
        ModelConfig cfg;
        cfg.adapter.clear();
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with an adapter-empty error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("adapter") != std::string::npos);
            }
        }
    }

    GIVEN("n_batch below 1") {
        ModelConfig cfg;
        cfg.adapter = "qwen35";
        cfg.n_batch = 0;
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with an n_batch error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("n_batch") != std::string::npos);
            }
        }
    }
}

SCENARIO("ModelsConfig validation — default tier must exist",
         "[config][validate][v2.3.10]") {
    GIVEN("a ModelsConfig whose default points at a missing tier") {
        ModelsConfig cfg;
        cfg.default_tier = "missing";
        TierConfig tier;
        tier.adapter = "qwen35";
        cfg.tiers["lead"] = tier;
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a 'default tier' error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("default tier") != std::string::npos);
            }
        }
    }

    GIVEN("an empty tiers map") {
        ModelsConfig cfg;
        cfg.default_tier = "anything";
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it passes (no tiers means no default-tier check)") {
                REQUIRE(err.empty());
            }
        }
    }
}
