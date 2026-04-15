// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file env_overrides_test.cpp
 * @brief BDD tests for ENTROPIC_* environment variable overrides.
 * @version 1.10.0
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <entropic/config/loader.h>
#include <cstdlib>

using Catch::Approx;

/**
 * @brief RAII guard for setting and restoring an environment variable.
 * @version 1.10.0
 * @internal
 */
struct EnvGuard {
    std::string name;
    std::string old_val;
    bool had_old;

    /**
     * @brief Set env var, save previous value.
     * @param n Variable name.
     * @param val Value to set.
     * @version 1.10.0
     * @internal
     */
    EnvGuard(const char* n, const char* val) : name(n) {
        const char* prev = std::getenv(n);
        had_old = (prev != nullptr);
        if (had_old) { old_val = prev; }
        setenv(n, val, 1);
    }

    ~EnvGuard() {
        if (had_old) {
            setenv(name.c_str(), old_val.c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

SCENARIO("ENTROPIC_LOG_LEVEL overrides config", "[config][env]") {
    GIVEN("a default config") {
        entropic::ParsedConfig config;
        config.log_level = "info";

        WHEN("ENTROPIC_LOG_LEVEL is set to debug") {
            EnvGuard guard("ENTROPIC_LOG_LEVEL", "debug");
            entropic::config::apply_env_overrides(config);

            THEN("log_level is overridden") {
                REQUIRE(config.log_level == "debug");
            }
        }
    }
}

SCENARIO("ENTROPIC_ROUTING__ENABLED overrides routing flag", "[config][env]") {
    GIVEN("a config with routing enabled") {
        entropic::ParsedConfig config;
        config.routing.enabled = true;

        WHEN("env var set to false") {
            EnvGuard guard("ENTROPIC_ROUTING__ENABLED", "false");
            entropic::config::apply_env_overrides(config);

            THEN("routing is disabled") {
                REQUIRE(config.routing.enabled == false);
            }
        }
    }
}

SCENARIO("ENTROPIC_COMPACTION__THRESHOLD_PERCENT overrides threshold",
         "[config][env]") {
    GIVEN("a config with default compaction threshold") {
        entropic::ParsedConfig config;
        config.compaction.threshold_percent = 75.0f;

        WHEN("env var set to 50") {
            EnvGuard guard("ENTROPIC_COMPACTION__THRESHOLD_PERCENT", "50");
            entropic::config::apply_env_overrides(config);

            THEN("threshold is updated") {
                REQUIRE(config.compaction.threshold_percent == Approx(50.0f));
            }
        }
    }
}

SCENARIO("Unset env vars leave config unchanged", "[config][env]") {
    GIVEN("a config with known values and no env vars set") {
        entropic::ParsedConfig config;
        config.log_level = "warn";
        config.routing.enabled = true;
        config.compaction.threshold_percent = 80.0f;

        WHEN("apply_env_overrides is called") {
            entropic::config::apply_env_overrides(config);

            THEN("all values remain unchanged") {
                REQUIRE(config.log_level == "warn");
                REQUIRE(config.routing.enabled == true);
                REQUIRE(config.compaction.threshold_percent == Approx(80.0f));
            }
        }
    }
}
