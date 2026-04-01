/**
 * @file test_grammar_orchestrator.cpp
 * @brief Tests for grammar registry integration in ModelOrchestrator.
 *
 * Tests cover: grammar_key resolution, raw grammar precedence,
 * frontmatter grammar normalization, and unknown key handling.
 *
 * These tests verify the resolution logic via the GrammarRegistry
 * directly, since the full orchestrator requires loaded models.
 *
 * @version 1.9.3
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/grammar_registry.h>
#include <entropic/types/config.h>

#include <filesystem>
#include <string>

namespace {

const std::string COMPACTOR_GBNF = R"(root ::= "compacted")";
const std::string SCRIBE_GBNF = R"(root ::= "scribed")";
const std::string RAW_GBNF = R"(root ::= "raw_override")";

/**
 * @brief Simulate orchestrator grammar_key resolution.
 *
 * Mirrors ModelOrchestrator::resolve_grammar_key() logic for
 * testing without a full orchestrator instance.
 *
 * @param params Generation params (mutated).
 * @param registry Grammar registry.
 * @param tier_grammar Optional tier config grammar value.
 * @utility
 * @version 1.9.3
 */
void resolve_grammar(
    entropic::GenerationParams& params,
    const entropic::GrammarRegistry& registry,
    const std::string& tier_grammar = "")
{
    if (!params.grammar.empty()) {
        return;
    }

    std::string key = params.grammar_key;

    if (key.empty() && !tier_grammar.empty()) {
        std::filesystem::path p(tier_grammar);
        key = (p.extension() == ".gbnf")
            ? p.stem().string() : tier_grammar;
    }

    if (key.empty()) {
        return;
    }

    std::string content = registry.get(key);
    if (!content.empty()) {
        params.grammar = content;
    }
}

} // anonymous namespace

SCENARIO("grammar_key resolved to content",
         "[grammar][orchestrator][resolve]")
{
    GIVEN("'compactor' is registered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("compactor", COMPACTOR_GBNF);

        WHEN("params has grammar_key = 'compactor'") {
            entropic::GenerationParams params;
            params.grammar_key = "compactor";
            resolve_grammar(params, reg);

            THEN("grammar field is set to compactor content") {
                REQUIRE(params.grammar == COMPACTOR_GBNF);
            }
        }
    }
}

SCENARIO("Raw grammar string takes precedence over grammar_key",
         "[grammar][orchestrator][precedence]")
{
    GIVEN("'compactor' is registered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("compactor", COMPACTOR_GBNF);

        WHEN("params has both grammar and grammar_key") {
            entropic::GenerationParams params;
            params.grammar = RAW_GBNF;
            params.grammar_key = "compactor";
            resolve_grammar(params, reg);

            THEN("raw grammar is preserved") {
                REQUIRE(params.grammar == RAW_GBNF);
            }
        }
    }
}

SCENARIO("Unknown grammar_key leaves unconstrained",
         "[grammar][orchestrator][resolve]")
{
    GIVEN("an empty registry") {
        entropic::GrammarRegistry reg;

        WHEN("params has grammar_key = 'nonexistent'") {
            entropic::GenerationParams params;
            params.grammar_key = "nonexistent";
            resolve_grammar(params, reg);

            THEN("grammar field remains empty") {
                REQUIRE(params.grammar.empty());
            }
        }
    }
}

SCENARIO("Frontmatter grammar with .gbnf extension normalized",
         "[grammar][orchestrator][frontmatter]")
{
    GIVEN("'compactor' is registered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("compactor", COMPACTOR_GBNF);

        WHEN("tier grammar is 'compactor.gbnf'") {
            entropic::GenerationParams params;
            resolve_grammar(params, reg, "compactor.gbnf");

            THEN("extension is stripped and grammar resolved") {
                REQUIRE(params.grammar == COMPACTOR_GBNF);
            }
        }
    }
}

SCENARIO("Frontmatter grammar without extension used as-is",
         "[grammar][orchestrator][frontmatter]")
{
    GIVEN("'scribe' is registered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("scribe", SCRIBE_GBNF);

        WHEN("tier grammar is 'scribe' (no extension)") {
            entropic::GenerationParams params;
            resolve_grammar(params, reg, "scribe");

            THEN("key is used directly and grammar resolved") {
                REQUIRE(params.grammar == SCRIBE_GBNF);
            }
        }
    }
}

SCENARIO("grammar_key takes precedence over frontmatter",
         "[grammar][orchestrator][precedence]")
{
    GIVEN("both 'compactor' and 'scribe' registered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("compactor", COMPACTOR_GBNF);
        reg.register_grammar("scribe", SCRIBE_GBNF);

        WHEN("params has grammar_key and tier has grammar") {
            entropic::GenerationParams params;
            params.grammar_key = "scribe";
            resolve_grammar(params, reg, "compactor.gbnf");

            THEN("grammar_key wins") {
                REQUIRE(params.grammar == SCRIBE_GBNF);
            }
        }
    }
}

SCENARIO("No grammar set leaves generation unconstrained",
         "[grammar][orchestrator][resolve]")
{
    GIVEN("'compactor' is registered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("compactor", COMPACTOR_GBNF);

        WHEN("params has no grammar and no grammar_key") {
            entropic::GenerationParams params;
            resolve_grammar(params, reg);

            THEN("grammar remains empty") {
                REQUIRE(params.grammar.empty());
            }
        }
    }
}

SCENARIO("Runtime-registered grammar usable in resolution",
         "[grammar][orchestrator][runtime]")
{
    GIVEN("a grammar registered at runtime") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("dynamic_eng", RAW_GBNF, "dynamic");

        WHEN("referenced by grammar_key") {
            entropic::GenerationParams params;
            params.grammar_key = "dynamic_eng";
            resolve_grammar(params, reg);

            THEN("grammar is resolved") {
                REQUIRE(params.grammar == RAW_GBNF);
            }
        }
    }
}

SCENARIO("Deregistered grammar no longer resolves",
         "[grammar][orchestrator][deregister]")
{
    GIVEN("a grammar was registered then deregistered") {
        entropic::GrammarRegistry reg;
        reg.register_grammar("temp", COMPACTOR_GBNF);
        reg.deregister("temp");

        WHEN("referenced by grammar_key") {
            entropic::GenerationParams params;
            params.grammar_key = "temp";
            resolve_grammar(params, reg);

            THEN("grammar field remains empty") {
                REQUIRE(params.grammar.empty());
            }
        }
    }
}
