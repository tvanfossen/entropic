// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_delegation_context_bleed.cpp
 * @brief Regression test for v2.0.6 — delegated-agent context bleed.
 *
 * Reproduces the failure mode described in
 * `.claude/proposals/ACTIVE/v2.0.6-delegated-context-bleed.md`:
 *
 * Before the fix, `save_prefix_to_cache()` captured the full prompt's
 * KV state but labeled it as "prefix only". On the next same-tier
 * generation (same system prompt → same cache key), the restore
 * leaked residual KV entries from the prior generation, which the
 * model's attention could read as context.
 *
 * This test drives two consecutive generations through the same
 * orchestrator/tier with identical system prompts but distinct user
 * content. If the KV truncation after state restore is missing or
 * wrong, the second generation's output will echo the first's
 * distinctive content.
 *
 * Requires: GPU, model on disk, prompt cache enabled (default).
 * Run: ctest -L model -R context-bleed
 *
 * @version 2.0.6
 */

#include "model_test_context.h"

#include <algorithm>
#include <cctype>
#include <string>

CATCH_REGISTER_LISTENER(ModelTestListener)

namespace {

/**
 * @brief Lowercase a copy of the input string.
 * @param s Input string.
 * @return Lowercased copy.
 * @utility
 * @version 2.0.6
 */
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

/**
 * @brief Check if haystack contains needle (case-insensitive substring).
 * @param haystack Text to search.
 * @param needle Substring to find.
 * @return true if needle appears in haystack (case-insensitive).
 * @utility
 * @version 2.0.6
 */
bool contains_ci(const std::string& haystack, const std::string& needle) {
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

} // anonymous namespace

SCENARIO("Consecutive same-tier generations do not leak content across "
         "KV cache restores",
         "[model][regression][v2.0.6][context-bleed]")
{
    GIVEN("an orchestrator with prompt cache enabled (default)") {
        REQUIRE(g_ctx.initialized);
        start_test_log("v2.0.6_delegation_context_bleed");

        auto params = test_gen_params();
        params.max_tokens = 160;
        params.enable_thinking = false;

        // Long system prompt to ensure the cache captures meaningful
        // prefix state (trivial prompts may token-count below the
        // cache store threshold).
        const std::string sys_prompt =
            "You are a concise research assistant. Answer every "
            "question using ONLY the information provided by the "
            "user in the CURRENT message. Do not reference prior "
            "messages. Keep responses short and factual.";

        WHEN("gen A defines a distinctive fictional term, then gen B "
             "asks an unrelated question with the same system prompt")
        {
            // Gen A: distinctive content that would pollute KV if the
            // fix is missing. The fictional term is deliberately
            // unusual so that any echo in gen B indicates bleed rather
            // than general model knowledge.
            const std::string marker_a = "QUIXOTIC_FLAMINGO_47";
            auto msgs_a = make_messages(
                sys_prompt,
                "Invent a one-sentence definition for the fictional "
                "term " + marker_a + ". Use the term verbatim in "
                "your answer.");
            auto r_a = g_ctx.orchestrator->generate(
                msgs_a, params, g_ctx.default_tier);

            // Sanity check: gen A should have used the marker. If it
            // didn't, the test setup is broken and subsequent
            // assertions on gen B are meaningless.
            REQUIRE(contains_ci(r_a.content, "QUIXOTIC"));

            // Gen B: same system prompt (→ same cache key → triggers
            // restore path), deliberately unrelated topic.
            auto msgs_b = make_messages(
                sys_prompt,
                "List the first three prime numbers in ascending "
                "order. Reply with just the numbers.");
            auto r_b = g_ctx.orchestrator->generate(
                msgs_b, params, g_ctx.default_tier);

            THEN("gen B does not echo the marker token from gen A") {
                CHECK_FALSE(contains_ci(r_b.content, "QUIXOTIC"));
                CHECK_FALSE(contains_ci(r_b.content, "FLAMINGO"));
            }

            THEN("gen B produces a plausible response to its own "
                 "question (contains at least one digit)")
            {
                bool has_digit = std::any_of(
                    r_b.content.begin(), r_b.content.end(),
                    [](unsigned char c) { return std::isdigit(c); });
                CHECK(has_digit);
            }

            end_test_log();
        }
    }
}

SCENARIO("Recall-style prompts do not surface prior-generation content",
         "[model][regression][v2.0.6][context-bleed]")
{
    GIVEN("an orchestrator with prompt cache enabled") {
        REQUIRE(g_ctx.initialized);
        start_test_log("v2.0.6_context_bleed_recall");

        auto params = test_gen_params();
        params.max_tokens = 160;
        params.enable_thinking = false;

        const std::string sys_prompt =
            "You are a stateless assistant. Each request is "
            "independent. You have no memory of prior interactions.";

        WHEN("gen A mentions a distinctive topic, then gen B asks "
             "whether any prior topic exists")
        {
            auto msgs_a = make_messages(
                sys_prompt,
                "Describe the TUI terminal interface in one sentence.");
            auto r_a = g_ctx.orchestrator->generate(
                msgs_a, params, g_ctx.default_tier);
            REQUIRE(!r_a.content.empty());

            auto msgs_b = make_messages(
                sys_prompt,
                "Is there anything I have asked you previously in "
                "this message? Reply yes or no and explain briefly.");
            auto r_b = g_ctx.orchestrator->generate(
                msgs_b, params, g_ctx.default_tier);

            THEN("gen B's response does not reference TUI "
                 "(the prior-generation topic)")
            {
                CHECK_FALSE(contains_ci(r_b.content, "TUI"));
                CHECK_FALSE(contains_ci(r_b.content, "terminal "
                                                    "interface"));
            }

            end_test_log();
        }
    }
}
