// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_reconnect_policy.cpp
 * @brief ReconnectPolicy unit tests.
 * @version 1.8.7
 */

#include <entropic/mcp/reconnect_policy.h>
#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace entropic;

TEST_CASE("First delay is approximately base_delay",
          "[reconnect_policy]") {
    ReconnectPolicy policy(1000, 60000, 5, 2.0);
    auto d = policy.delay_ms(0);
    // Base is 1000, jitter adds up to 10%
    REQUIRE(d >= 1000);
    REQUIRE(d <= 1100);
}

TEST_CASE("Delay increases exponentially",
          "[reconnect_policy]") {
    ReconnectPolicy policy(1000, 60000, 5, 2.0);
    auto d0 = policy.delay_ms(0);
    auto d1 = policy.delay_ms(1);
    auto d2 = policy.delay_ms(2);
    // Each delay roughly doubles (within jitter)
    REQUIRE(d1 > d0);
    REQUIRE(d2 > d1);
}

TEST_CASE("Delay capped at max_delay",
          "[reconnect_policy]") {
    ReconnectPolicy policy(1000, 5000, 5, 2.0);
    auto d = policy.delay_ms(100);
    // Capped at 5000 + 10% jitter
    REQUIRE(d <= 5500);
}

TEST_CASE("Jitter adds variance",
          "[reconnect_policy]") {
    ReconnectPolicy policy(1000, 60000, 5, 2.0);
    std::set<uint32_t> values;
    for (int i = 0; i < 20; ++i) {
        values.insert(policy.delay_ms(3));
    }
    // With jitter, at least 2 distinct values in 20 samples
    REQUIRE(values.size() >= 2);
}

TEST_CASE("Exhausted at max_retries",
          "[reconnect_policy]") {
    ReconnectPolicy policy(1000, 60000, 5, 2.0);
    REQUIRE_FALSE(policy.exhausted(4));
    REQUIRE(policy.exhausted(5));
    REQUIRE(policy.exhausted(6));
}

TEST_CASE("Zero max_retries never exhausts",
          "[reconnect_policy]") {
    ReconnectPolicy policy(1000, 60000, 0, 2.0);
    REQUIRE_FALSE(policy.exhausted(0));
    REQUIRE_FALSE(policy.exhausted(100));
    REQUIRE_FALSE(policy.exhausted(999999));
}

TEST_CASE("Construct from ReconnectConfig",
          "[reconnect_policy]") {
    ReconnectConfig cfg;
    cfg.base_delay_ms = 500;
    cfg.max_delay_ms = 10000;
    cfg.max_retries = 3;
    cfg.backoff_factor = 1.5;

    ReconnectPolicy policy(cfg);
    auto d = policy.delay_ms(0);
    REQUIRE(d >= 500);
    REQUIRE(d <= 550);
    REQUIRE(policy.exhausted(3));
}
