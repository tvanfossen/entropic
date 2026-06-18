// SPDX-License-Identifier: Apache-2.0
/**
 * @file mtp_envelope_test.cpp
 * @brief gh#108 (v2.9.1): CPU pin for the MTP fail-fast/fail-loud envelope.
 *
 * mtp_unsupported_reason is the pure decision behind the loud errors the
 * backend raises when speculative.mtp is enabled outside MTP's correct
 * envelope (greedy / unconstrained / no-tools / non-streaming). Keeping the
 * logic in a pure function lets this run on CPU with no GPU/model; the model
 * tests verify the backend actually consults it and propagates the error.
 */

#include <catch2/catch_test_macros.hpp>

#include "mtp_envelope.h"

using entropic::mtp_unsupported_reason;

namespace {
// Safe envelope: greedy, unconstrained, no tools, non-streaming.
std::string safe() { return mtp_unsupported_reason(0.0f, false, false, false); }
}  // namespace

TEST_CASE("gh#108 MTP envelope: the safe case yields no error", "[mtp][envelope]") {
    REQUIRE(safe().empty());
}

TEST_CASE("gh#108 MTP envelope: each incompatible condition fails loud",
          "[mtp][envelope]") {
    SECTION("temperature > 0 (lossless only at greedy)") {
        auto r = mtp_unsupported_reason(0.7f, false, false, false);
        REQUIRE_FALSE(r.empty());
        REQUIRE(r.find("temperature=0") != std::string::npos);
    }
    SECTION("grammar active") {
        auto r = mtp_unsupported_reason(0.0f, true, false, false);
        REQUIRE_FALSE(r.empty());
        REQUIRE(r.find("grammar") != std::string::npos);
    }
    SECTION("tools staged") {
        auto r = mtp_unsupported_reason(0.0f, false, true, false);
        REQUIRE_FALSE(r.empty());
        REQUIRE(r.find("tool") != std::string::npos);
    }
    SECTION("streaming") {
        auto r = mtp_unsupported_reason(0.0f, false, false, true);
        REQUIRE_FALSE(r.empty());
        REQUIRE(r.find("streaming") != std::string::npos);
    }
}

TEST_CASE("gh#108 MTP envelope: every message is actionable (mentions mtp)",
          "[mtp][envelope]") {
    // Each non-empty reason tells the consumer how to correct it.
    for (auto r : {mtp_unsupported_reason(0.7f, false, false, false),
                   mtp_unsupported_reason(0.0f, true, false, false),
                   mtp_unsupported_reason(0.0f, false, true, false),
                   mtp_unsupported_reason(0.0f, false, false, true)}) {
        REQUIRE_FALSE(r.empty());
        REQUIRE(r.find("mtp") != std::string::npos);  // names the knob to change
    }
}

TEST_CASE("gh#108 MTP envelope: temperature is the highest-precedence gate",
          "[mtp][envelope]") {
    // When several conditions hold at once, temp>0 is reported first — a
    // deterministic, stable message (not dependent on check ordering drift).
    auto r = mtp_unsupported_reason(0.7f, true, true, true);
    REQUIRE(r.find("temperature=0") != std::string::npos);
}

TEST_CASE("gh#108 MTP envelope: temperature boundary is exactly 0", "[mtp][envelope]") {
    REQUIRE(mtp_unsupported_reason(0.0f, false, false, false).empty());
    // The smallest positive temperature already trips the gate (greedy-only).
    REQUIRE_FALSE(mtp_unsupported_reason(1e-6f, false, false, false).empty());
}
