/**
 * @file test_grammar_constraint.cpp
 * @brief BDD subsystem test — GBNF grammar constrains output to valid JSON.
 *
 * Validates that grammar-constrained generation produces valid JSON.
 * Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test 5: Grammar Constraint ──────────────────────────────

SCENARIO("GBNF grammar constrains output to valid JSON",
         "[model][test5]")
{
    GIVEN("a configured engine with a JSON grammar registered") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test5_grammar_constraint");
        auto& grammar = g_ctx.orchestrator->grammar_registry();
        bool has_json = grammar.has("json");
        if (!has_json) {
            static const char* json_gbnf =
                "root ::= \"{\" ws members ws \"}\"\n"
                "members ::= pair (\",\" ws pair)*\n"
                "pair ::= string \":\" ws value\n"
                "value ::= string | number | \"true\" | \"false\"\n"
                "string ::= \"\\\"\" [a-zA-Z0-9_ ]* \"\\\"\"\n"
                "number ::= [0-9]+\n"
                "ws ::= [ \\t\\n]*\n";
            grammar.register_grammar("json", json_gbnf);
        }
        auto params = test_gen_params();
        params.grammar_key = "json";
        params.max_tokens = 128;
        auto messages = make_messages(
            "You are a helpful assistant.",
            "Give me a JSON object with name and age fields");

        WHEN("I generate with grammar_key set") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);

            THEN("the output is valid JSON with expected keys") {
                auto parsed = json::parse(
                    result.content, nullptr, false);
                REQUIRE_FALSE(parsed.is_discarded());
                CHECK(parsed.contains("name"));
                CHECK(parsed.contains("age"));
                end_test_log();
            }
        }
    }
}
