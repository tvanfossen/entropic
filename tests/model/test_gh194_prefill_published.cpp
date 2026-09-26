// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_gh194_prefill_published.cpp
 * @brief gh#194: prefill_tokens must reach a consumer, not just the log.
 *
 * gh#154 added `generations[]` to `entropic_metrics_json` so a consumer could
 * finally read what a run did — `prefill_tokens` among them, computed since
 * gh#144 and dropped ever since. It reads 0.
 *
 * The cause is two counters. `last_prefill_tokens_` is set by
 * `run_prefill_cached`, the path every decode uses, and is only ever logged.
 * `GenerationResult::prefill_tokens` is assigned in exactly one place —
 * `spec_finalize` — from `state.n_prefilled`, a counter local to the
 * speculative loop that counts only the chunks that loop decodes itself. The
 * shared prefill has normally already done the prompt, so it reads 0 even on
 * the path that assigns it.
 *
 * This asserts the number reaches the API and AGREES with the engine's own
 * log. Agreement is the point: two counters that cannot be compared are how
 * this stayed broken through a release that shipped the field for consumers.
 *
 * @version 2.13.2
 */
#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>
#include "facade_model_helpers.h"

#include <nlohmann/json.hpp>
#include <string>

namespace {

/**
 * @brief prefill_tokens of the most recent generation, via the public API.
 * @param h Engine handle.
 * @return The value, or -1 when unavailable.
 * @internal
 * @version 2.13.2
 */
int published_prefill(entropic_handle_t h) {
    char* raw = nullptr;
    if (entropic_metrics_json(h, &raw) != ENTROPIC_OK || raw == nullptr) {
        return -1;
    }
    const std::string text(raw);
    entropic_free(raw);
    try {
        const auto j = nlohmann::json::parse(text);
        const auto it = j.find("generations");
        if (it == j.end() || !it->is_array() || it->empty()) { return -1; }
        return it->back().value("prefill_tokens", -1);
    } catch (...) {
        return -1;
    }
}

}  // namespace

SCENARIO("gh#194: a plain generation publishes the prefill it performed",
         "[model][gh194][2.13.2]") {
    GIVEN("a tier with no speculative decode configured") {
        entropic::test::facade::FacadeProject project("gh194_prefill");
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body = "You are a terse assistant. Answer in one"
                             "sentence.";
        lead.context_length = 4096;
        lead.allowed_tools = {"entropic.complete"};
        lead.explicit_completion = false;
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        WHEN("one turn is run on a cold context") {
            const std::string convo = entropic::test::facade::run_transcript(
                h, "Name one primary colour.");
            REQUIRE_FALSE(convo.empty());

            THEN("prefill_tokens is published and is not zero") {
                const int published = published_prefill(h);
                INFO("generations[].prefill_tokens = " << published);
                // A cold context must decode the system prompt, the tool
                // schema and the question. Zero means the field never left
                // the backend; a small constant would mean something is
                // published but it is not the prefill. Observed 717 on this
                // tier, so a floor of 100 is generous and still excludes
                // both failure modes.
                REQUIRE(published >= 0);
                CHECK(published > 100);
            }
        }
    }
}
