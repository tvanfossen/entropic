// SPDX-License-Identifier: LGPL-3.0-or-later
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
}
