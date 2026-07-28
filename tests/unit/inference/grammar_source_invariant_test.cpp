// SPDX-License-Identifier: Apache-2.0
/**
 * @file grammar_source_invariant_test.cpp
 * @brief gh#134: every grammar source must reach the sampler, and only one
 *        may win.
 *
 * @par Why this file exists
 * Entropic accumulated grammar sources one at a time, each arriving with a
 * bespoke regression test written after its own bug:
 *
 *   - `params.grammar`      — grammar_engagement_test.cpp (gh#95)
 *   - `params.grammar_key`  — test_grammar_constraint.cpp
 *   - identity `grammar:`   — test_gh95_identity_grammar.cpp
 *   - `params.grammar`+MTP  — test_gh108_mtp_stream_grammar.cpp (gh#108)
 *
 * All four enter through `GenerationParams::grammar`. Nobody wrote the
 * GENERIC invariant, so a source could be added unwired and the suite would
 * stay green — which is exactly what happened: `common_chat_templates_apply`
 * derives a tool-call GBNF from the staged tool schemas and
 * `render_with_tools` discarded it, leaving tools-staged tiers decoding
 * unconstrained and `tool_choice: REQUIRED` inert.
 *
 * @par What this pins
 * The precedence rule, which is the only part of the wiring carrying
 * judgment. The two sources cannot compose — they constrain output to
 * different languages — so exactly one must win and the collision must be
 * reportable rather than silently resolved.
 *
 * The type distinction is enforced at the application site rather than here:
 * a tool-call grammar is applied as COMMON_GRAMMAR_TYPE_TOOL_CALLS, never
 * _USER, because `common_grammar_needs_prefill()` is true only for the former
 * and the model's output begins mid-template. Applied as _USER the grammar
 * would reject from the first token.
 *
 * @version 2.10.4
 */

#include "grammar_source.h"

#include <catch2/catch_test_macros.hpp>

using entropic::GrammarSource;
using entropic::grammar_sources_collide;
using entropic::resolve_grammar_source;

SCENARIO("gh#134 every declared grammar source is wired to the sampler",
         "[gh134][grammar][inference][cpu][invariant]")
{
    // THE STANDING INVARIANT, and the reason this file exists.
    //
    // Three times now a grammar source reached the engine and never reached
    // the sampler: gh#95 (identity grammar dropped in the facade), gh#108
    // (params.grammar not propagated to MTP), gh#134 (the render's tool-call
    // grammar discarded outright). Each got a bespoke regression test for its
    // own source; nobody asserted the general property, so source N+1 shipped
    // unwired every time.
    //
    // GrammarSource::count is the sentinel. Adding a source without extending
    // resolve_grammar_source AND this test fails here — deliberately.
    GIVEN("the declared set of grammar sources") {
        THEN("it is exactly the set this test knows how to exercise") {
            // Bump ONLY together with a new case below. If this fires, a
            // source was added without proving it constrains decoding.
            CHECK(entropic::grammar_source_count() == 3);
        }

        THEN("every non-none source is reachable from resolve_grammar_source") {
            // Each source must be producible — an enum value no input can
            // yield is a source that is declared but not wired.
            CHECK(resolve_grammar_source("g", "") == GrammarSource::request);
            CHECK(resolve_grammar_source("", "g") == GrammarSource::tool_call);
            CHECK(resolve_grammar_source("", "") == GrammarSource::none);
        }
    }
}

SCENARIO("gh#134 exactly one grammar source constrains a decode",
         "[gh134][grammar][inference][cpu][invariant]")
{
    GIVEN("neither source set") {
        THEN("the decode is unconstrained") {
            CHECK(resolve_grammar_source("", "") == GrammarSource::none);
            CHECK_FALSE(grammar_sources_collide("", ""));
        }
    }

    GIVEN("only a request grammar — the source that already worked") {
        THEN("the request grammar wins") {
            CHECK(resolve_grammar_source("root ::= \"x\"", "")
                  == GrammarSource::request);
            CHECK_FALSE(grammar_sources_collide("root ::= \"x\"", ""));
        }
    }

    GIVEN("only a tool-call grammar — the source discarded before v2.10.4") {
        THEN("it constrains the decode instead of being dropped") {
            // RED before gh#134: the render's grammar never left
            // render_with_tools, so this case could not arise at all and a
            // tools-staged tier decoded free-form.
            CHECK(resolve_grammar_source("", "root ::= \"call\"")
                  == GrammarSource::tool_call);
            CHECK_FALSE(grammar_sources_collide("", "root ::= \"call\""));
        }
    }

    GIVEN("both sources set") {
        const std::string req = "root ::= \"x\"";
        const std::string tool = "root ::= \"call\"";

        THEN("the explicit request grammar wins over the implied one") {
            // Staging tools implies a grammar; passing one is an explicit
            // instruction. The explicit intent takes precedence.
            CHECK(resolve_grammar_source(req, tool) == GrammarSource::request);
        }

        THEN("the collision is reportable, not silently resolved") {
            // The losing constraint's absence would otherwise be
            // undiagnosable — the caller sees tools staged and assumes they
            // are structurally enforced when they are not.
            CHECK(grammar_sources_collide(req, tool));
        }
    }
}
