/**
 * @file test_logprob_evaluation.cpp
 * @brief BDD subsystem test — evaluation produces valid log-probabilities.
 *
 * Validates logprob evaluation returns correct counts and sane ranges.
 * Uses ModelOrchestrator directly (not AgentEngine).
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test 7: Logprob Evaluation ──────────────────────────────

SCENARIO("Evaluation produces valid log-probabilities",
         "[model][test7]")
{
    GIVEN("an ACTIVE model and a tokenized sequence") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test7_logprob_evaluation");
        auto* backend = g_ctx.orchestrator->get_backend(
            g_ctx.default_tier);
        REQUIRE(backend != nullptr);
        auto tokens = backend->tokenize_text(
            "The quick brown fox jumps over the lazy dog.");
        REQUIRE(tokens.size() >= 4);

        WHEN("logprob evaluation is called") {
            auto result = backend->evaluate_logprobs(
                tokens.data(),
                static_cast<int>(tokens.size()));

            THEN("logprobs are correct count, negative, and perplexity is sane") {
                REQUIRE(result.n_logprobs
                        == static_cast<int>(tokens.size()) - 1);
                REQUIRE(result.logprobs.size()
                        == static_cast<size_t>(result.n_logprobs));

                for (float lp : result.logprobs) {
                    CHECK(lp <= 0.0f);
                }

                CHECK(result.perplexity > 1.0f);
                CHECK(result.perplexity < 100.0f);
                end_test_log();
            }
        }
    }
}
