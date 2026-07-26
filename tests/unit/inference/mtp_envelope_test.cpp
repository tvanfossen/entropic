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
 * exact-match accept step is already lossless at any temperature. v2.10.0
 * (gh#108): grammar is no longer guarded — to_common_sampling now propagates
 * params.grammar to common_params_sampling so the MTP sampler enforces GBNF.
 * Keeping the logic pure lets this run on CPU; the model tests verify the
 * backend consults it and propagates the error.
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

TEST_CASE("gh#108 MTP envelope: all conditions are now open (full envelope)",
          "[mtp][envelope]") {
    // v2.10.0 (gh#108): grammar and streaming guards both removed. The
    // full envelope is now: any temperature, grammar OK, streaming OK.
    SECTION("grammar alone — not guarded") {
        REQUIRE(mtp_unsupported_reason(0.0f, true, false).empty());
    }
    SECTION("streaming alone — not guarded (StreamThinkFilter handles it)") {
        REQUIRE(mtp_unsupported_reason(0.0f, false, true).empty());
    }
    SECTION("grammar + streaming — both open") {
        REQUIRE(mtp_unsupported_reason(0.7f, true, true).empty());
    }
}

TEST_CASE("gh#108 MTP envelope: no condition produces an error (envelope fully open)",
          "[mtp][envelope]") {
    // v2.10.0: all guards removed. mtp_unsupported_reason always returns "".
    REQUIRE(mtp_unsupported_reason(0.0f, false, false).empty());
    REQUIRE(mtp_unsupported_reason(0.0f, true, false).empty());
    REQUIRE(mtp_unsupported_reason(0.0f, false, true).empty());
    REQUIRE(mtp_unsupported_reason(0.7f, true, true).empty());
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

// ── gh#108 (v2.10.0): grammar is no longer a guard ──────────────────────────

TEST_CASE("gh#108: grammar is not guarded (propagated via to_common_sampling)",
          "[mtp][envelope][gh108-grammar][2.10.0]") {
    // RED before fix: mtp_unsupported_reason returns a non-empty error when
    // has_grammar=true (the v2.9.4 guard blocked MTP+grammar).
    // GREEN after fix: to_common_sampling propagates params.grammar to
    // common_params_sampling so the guard is no longer needed and removed.
    REQUIRE(mtp_unsupported_reason(0.0f, true, false).empty());
}

// ── gh#108 (v2.10.0): streaming is no longer a hard guard ───────────────────

TEST_CASE("gh#108: streaming is not guarded (StreamThinkFilter handles live strip)",
          "[mtp][envelope][gh108-streaming][2.10.0]") {
    // RED before fix: mtp_unsupported_reason returns a non-empty error when
    // streaming=true (the v2.9.1 guard blocked MTP+streaming because
    // strip_thinking_channels is post-buffer only). GREEN after fix:
    // generate_streaming now wraps on_token with StreamThinkFilter so
    // incremental stripping is safe; the guard is removed.
    REQUIRE(mtp_unsupported_reason(0.0f, false, true).empty());
}

// ── gh#107 (v2.10.0): MTP head detection ────────────────────────────────────

TEST_CASE("gh#107: looks_like_mtp_head detects small-layer GGUFs",
          "[mtp][envelope][gh107][2.10.0]") {
    // An MTP head GGUF (e.g. Gemma4 assistant head) has 1-2 transformer
    // layers. A classical draft model has at least 4. The predicate guards
    // try_speculative_route_streaming: routing a head GGUF to the classical
    // separate-draft path crashes in fattn.cu — fail loud instead.
    //
    // RED before fix: looks_like_mtp_head is not defined (compile failure).
    REQUIRE(entropic::looks_like_mtp_head(1));   // gemma4 head = 1 layer
    REQUIRE(entropic::looks_like_mtp_head(2));   // 2-layer head GGUF
    REQUIRE_FALSE(entropic::looks_like_mtp_head(0));   // invalid
    REQUIRE_FALSE(entropic::looks_like_mtp_head(4));   // small but not an MTP head
    REQUIRE_FALSE(entropic::looks_like_mtp_head(32));  // normal draft model
}

TEST_CASE("gh#107: looks_like_mtp_head message mentions mtp flag",
          "[mtp][envelope][gh107][2.10.0]") {
    // The guard in the orchestrator must emit an actionable message that
    // tells the consumer to set speculative.mtp: true. Verified at the
    // predicate level — the orchestrator wiring is covered by model tests.
    std::string reason = entropic::mtp_head_classical_path_error(1);
    REQUIRE_FALSE(reason.empty());
    REQUIRE(reason.find("mtp") != std::string::npos);
    REQUIRE(reason.find("layer") != std::string::npos);
}
