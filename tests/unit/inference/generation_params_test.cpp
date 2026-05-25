// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_generation_params.cpp
 * @brief Tests for GenerationParams struct defaults.
 * @version 2.0.6-rc16
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/types/config.h>

SCENARIO("GenerationParams defaults", "[params][defaults]") {
    GIVEN("a default-constructed GenerationParams") {
        entropic::GenerationParams params;

        THEN("defaults match specification") {
            REQUIRE(params.max_tokens == 4096);
            REQUIRE(params.temperature == 0.7f);
            REQUIRE(params.top_p == 0.9f);
            REQUIRE(params.top_k == 40);
            REQUIRE(params.repeat_penalty == 1.1f);
            REQUIRE(params.min_p == 0.0f);  // gh#23: 0.0 = disabled sentinel
            REQUIRE(params.presence_penalty == 0.0f);  // gh#23 v2.3.14: 0.0 = disabled
            REQUIRE(params.reasoning_budget == -1);
            REQUIRE(params.enable_thinking == true);
            REQUIRE(params.grammar.empty());
            REQUIRE(params.stop.empty());
            REQUIRE(params.logprobs == 0);
            REQUIRE(params.seed == -1);  // P2-14: -1 = random
        }
    }
}

// ── gh#23 v2.3.10: min_p sampler knob ─────────────────────

SCENARIO("min_p field round-trips through GenerationParams",
         "[params][min_p][gh23]")
{
    GIVEN("a default GenerationParams") {
        entropic::GenerationParams p;
        THEN("min_p defaults to 0.0 (disabled sentinel)") {
            REQUIRE(p.min_p == 0.0f);
        }
    }

    GIVEN("min_p set to a typical productive value") {
        entropic::GenerationParams p;
        p.min_p = 0.05f;
        THEN("reads back exactly") {
            REQUIRE(p.min_p == 0.05f);
        }
    }

    GIVEN("min_p set at lower boundary (0.0)") {
        entropic::GenerationParams p;
        p.min_p = 0.0f;
        THEN("stays at disabled sentinel and chain gate (> 0) skips it") {
            REQUIRE(p.min_p == 0.0f);
            REQUIRE_FALSE(p.min_p > 0.0f);  // mirrors create_sampler gate
        }
    }

    GIVEN("min_p set at upper boundary (1.0)") {
        entropic::GenerationParams p;
        p.min_p = 1.0f;
        THEN("stores at boundary — degenerate but accepted, no crash") {
            REQUIRE(p.min_p == 1.0f);
            REQUIRE(p.min_p > 0.0f);  // sampler gate would fire
        }
    }

    GIVEN("min_p set to a value greater than 1.0 (over-spec)") {
        entropic::GenerationParams p;
        p.min_p = 1.5f;
        THEN("stored without clamping at struct level — llama.cpp accepts") {
            // Filter-everything behavior is the caller's problem; the
            // struct must not silently rewrite caller intent. gh#23
            // adversarial review keeps validation/clamping at the
            // schema layer, not at the field.
            REQUIRE(p.min_p == 1.5f);
        }
    }

    GIVEN("min_p set to a negative value (out-of-spec)") {
        entropic::GenerationParams p;
        p.min_p = -0.1f;
        THEN("stored without rewrite — chain gate (> 0) treats as disabled") {
            REQUIRE(p.min_p == -0.1f);
            REQUIRE_FALSE(p.min_p > 0.0f);  // mirrors create_sampler gate
        }
    }
}

SCENARIO("min_p coexists with top_p / top_k / temperature without aliasing",
         "[params][min_p][gh23][independence]")
{
    GIVEN("min_p and top_p both set to distinct values") {
        entropic::GenerationParams p;
        p.top_p = 0.92f;
        p.min_p = 0.05f;
        THEN("each field reads back its own value (no struct aliasing)") {
            REQUIRE(p.top_p == 0.92f);
            REQUIRE(p.min_p == 0.05f);
        }
    }

    GIVEN("min_p set, then top_p / top_k / temp / repeat_penalty mutated") {
        entropic::GenerationParams p;
        p.min_p = 0.07f;
        p.top_p = 0.5f;
        p.top_k = 20;
        p.temperature = 1.3f;
        p.repeat_penalty = 1.05f;
        THEN("min_p survives every adjacent mutation") {
            REQUIRE(p.min_p == 0.07f);
        }
        THEN("adjacent fields are unaffected by min_p's value") {
            REQUIRE(p.top_p == 0.5f);
            REQUIRE(p.top_k == 20);
            REQUIRE(p.temperature == 1.3f);
            REQUIRE(p.repeat_penalty == 1.05f);
        }
    }
}

SCENARIO("min_p default preserves pre-v2.3.10 sampler-chain shape exactly",
         "[params][min_p][gh23][backward-compat]")
{
    // Backward-compat contract: a caller that does NOT set min_p must
    // produce a sampler chain bit-identical to v2.3.9. The chain's
    // min_p sampler is gated by `params.min_p > 0.0f` in
    // LlamaCppBackend::create_sampler. This test pins that gate
    // condition on the field defaults so a future default-change
    // (e.g. someone setting `min_p = 0.05f` as the new default)
    // would have to break this test deliberately.
    GIVEN("a default-constructed GenerationParams") {
        entropic::GenerationParams p;
        THEN("the create_sampler gate (min_p > 0.0f) is false") {
            REQUIRE_FALSE(p.min_p > 0.0f);
        }
    }
}

// ── P2-14: seed field ─────────────────────────────────────

SCENARIO("seed field round-trips through GenerationParams",
         "[params][seed][P2-14]")
{
    GIVEN("a GenerationParams with seed set to 42") {
        entropic::GenerationParams p;
        p.seed = 42;

        THEN("seed reads back correctly") {
            REQUIRE(p.seed == 42);
        }
    }

    GIVEN("a default GenerationParams") {
        entropic::GenerationParams p;
        THEN("seed defaults to -1 (random)") {
            REQUIRE(p.seed == -1);
        }
    }
}

// ── gh#23 v2.3.14: presence_penalty sampler knob ──────────

SCENARIO("presence_penalty field round-trips through GenerationParams",
         "[params][presence_penalty][gh23]")
{
    GIVEN("a default GenerationParams") {
        entropic::GenerationParams p;
        THEN("presence_penalty defaults to 0.0 (disabled sentinel)") {
            REQUIRE(p.presence_penalty == 0.0f);
        }
    }

    GIVEN("presence_penalty set to a typical productive value") {
        entropic::GenerationParams p;
        p.presence_penalty = 0.6f;
        THEN("the value reads back unchanged") {
            REQUIRE(p.presence_penalty == 0.6f);
        }
    }

    GIVEN("presence_penalty at boundary values") {
        entropic::GenerationParams p;
        WHEN("set to 0.0") { p.presence_penalty = 0.0f;
            THEN("reads 0.0") { REQUIRE(p.presence_penalty == 0.0f); } }
        WHEN("set to 1.0") { p.presence_penalty = 1.0f;
            THEN("reads 1.0") { REQUIRE(p.presence_penalty == 1.0f); } }
        WHEN("set to 2.0 (upper typical range)") { p.presence_penalty = 2.0f;
            THEN("reads 2.0") { REQUIRE(p.presence_penalty == 2.0f); } }
        WHEN("set to -0.5 (negative, model invokes 'discourage absent')") {
            p.presence_penalty = -0.5f;
            THEN("reads -0.5 — schema layer is responsible for clamping") {
                REQUIRE(p.presence_penalty == -0.5f);
            }
        }
    }
}

SCENARIO("presence_penalty is independent of other penalty/sampling knobs",
         "[params][presence_penalty][gh23]")
{
    GIVEN("presence_penalty modified") {
        entropic::GenerationParams p;
        p.presence_penalty = 0.5f;
        THEN("repeat_penalty / min_p / top_p / top_k / temperature stay at default") {
            REQUIRE(p.repeat_penalty == 1.1f);
            REQUIRE(p.min_p == 0.0f);
            REQUIRE(p.top_p == 0.9f);
            REQUIRE(p.top_k == 40);
            REQUIRE(p.temperature == 0.7f);
        }
    }
}

SCENARIO("presence_penalty default still gates the penalties sampler OFF "
         "alongside default repeat_penalty",
         "[params][presence_penalty][gh23][backward-compat]")
{
    // The penalties-sampler gate in create_sampler is now
    // `if (repeat_penalty != 1.0 || presence_penalty > 0.0)`.
    // With BOTH at defaults, the gate must stay false so the chain
    // is bit-identical to pre-v2.3.14.
    GIVEN("a default GenerationParams") {
        entropic::GenerationParams p;
        THEN("repeat_penalty == 1.1 (default, != 1.0 → sampler stays ON for back-compat)") {
            REQUIRE(p.repeat_penalty == 1.1f);
            REQUIRE(p.presence_penalty == 0.0f);
        }
    }
    GIVEN("repeat_penalty explicitly set to 1.0 AND presence_penalty default") {
        entropic::GenerationParams p;
        p.repeat_penalty = 1.0f;
        THEN("both penalty knobs are now disabled — sampler stage skipped") {
            REQUIRE(p.repeat_penalty == 1.0f);
            REQUIRE(p.presence_penalty == 0.0f);
        }
    }
}

SCENARIO("ModelState enum values", "[types][enums]") {
    THEN("ModelState maps to C enum values") {
        REQUIRE(static_cast<int>(entropic::ModelState::COLD) == 0);
        REQUIRE(static_cast<int>(entropic::ModelState::WARM) == 1);
        REQUIRE(static_cast<int>(entropic::ModelState::ACTIVE) == 2);
    }
}
