// SPDX-License-Identifier: Apache-2.0
/**
 * @file mtp_envelope_test.cpp
 * @brief gh#108: CPU pin for the MTP fail-fast/fail-loud envelope predicate.
 *
 * mtp_unsupported_reason is the pure decision behind the loud errors the backend
 * raises when speculative.mtp is enabled outside MTP's correct envelope. v2.9.2:
 * TOOLS are no longer guarded (MTP+tools is lossless-correct; stops now honored).
 * v2.9.3: flash attention is no longer guarded either — the extern/llama.cpp pin
 * is past upstream #25148, which fixed the GQA-2 flash-attn abort. v2.9.4:
 * temperature is no longer guarded — the draft proposal is a deterministic
 * point mass (see mtp_envelope.h's file-level doc comment), so the existing
 * exact-match accept step is already lossless at any temperature. Keeping the
 * logic pure lets this run on CPU; the model tests verify the backend consults
 * it and propagates the error.
 */

#include <catch2/catch_test_macros.hpp>

#include "mtp_envelope.h"

using entropic::mtp_unsupported_reason;

namespace {
// Safe envelope: greedy, unconstrained, non-streaming.
std::string safe() { return mtp_unsupported_reason(0.0f, false, false); }
}  // namespace

TEST_CASE("gh#108 MTP envelope: the safe case yields no error", "[mtp][envelope]") {
    REQUIRE(safe().empty());
}

TEST_CASE("gh#108 MTP envelope: tools are NOT guarded (lossless-correct)",
          "[mtp][envelope]") {
    // v2.9.2: tools were dropped from the signature entirely. The safe case has
    // no tools concept; a greedy/unconstrained/non-streaming request passes
    // regardless of whether tools are staged at the call site.
    REQUIRE(safe().empty());
}

TEST_CASE("gh#108 MTP envelope: each incompatible condition fails loud",
          "[mtp][envelope]") {
    SECTION("grammar active") {
        auto r = mtp_unsupported_reason(0.0f, true, false);
        REQUIRE_FALSE(r.empty());
        REQUIRE(r.find("grammar") != std::string::npos);
    }
    SECTION("streaming") {
        auto r = mtp_unsupported_reason(0.0f, false, true);
        REQUIRE_FALSE(r.empty());
        REQUIRE(r.find("streaming") != std::string::npos);
    }
}

TEST_CASE("gh#108 MTP envelope: every message is actionable (mentions mtp)",
          "[mtp][envelope]") {
    for (auto r : {mtp_unsupported_reason(0.0f, true, false),
                   mtp_unsupported_reason(0.0f, false, true)}) {
        REQUIRE_FALSE(r.empty());
        REQUIRE(r.find("mtp") != std::string::npos);  // names the knob to change
    }
}

TEST_CASE("gh#108 MTP envelope: grammar is the highest-precedence gate",
          "[mtp][envelope]") {
    // v2.9.4: temperature is no longer a gate at all, so grammar (checked
    // first in mtp_unsupported_reason) wins over streaming when both are set.
    auto r = mtp_unsupported_reason(0.7f, true, true);
    REQUIRE(r.find("grammar") != std::string::npos);
}

TEST_CASE("gh#108 MTP envelope: temperature is not guarded at any value",
          "[mtp][envelope]") {
    // v2.9.4: dropped as a gate — the draft proposal is a deterministic point
    // mass, so the existing exact-match accept step is lossless regardless.
    REQUIRE(mtp_unsupported_reason(0.0f, false, false).empty());
    REQUIRE(mtp_unsupported_reason(1e-6f, false, false).empty());
    REQUIRE(mtp_unsupported_reason(0.7f, false, false).empty());
    REQUIRE(mtp_unsupported_reason(2.0f, false, false).empty());
}
