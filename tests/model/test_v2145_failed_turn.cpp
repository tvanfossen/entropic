// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v2145_failed_turn.cpp
 * @brief gh#209/#212 (v2.14.5): a turn the engine fails is reported as a
 *        failure, with only THIS turn's text — over the real bridge.
 *
 * The emergent multi-turn test. One session, three asks through the MCP
 * bridge's `ask` tool:
 *   1. under a free tier, the model answers (and is told a word);
 *   2. under a tier that owes explicit completion but may call NO tool
 *      (`allowed_tools: []`), the turn cannot complete — the engine fails
 *      it deterministically, whatever the model writes;
 *   3. back under the free tier, the session recalls the word.
 * Before v2.14.5 ask 2 came back as an ordinary answer: turn 1's text
 * (gh#209) or a completion call written as text (gh#212), with no error.
 *
 * Requires: GPU, gemma-4-E2B QAT GGUF on disk.
 * Run: inv test --model --model-only --filter v2145
 *
 * @version 2.14.5
 */

#include "facade_model_helpers.h"
#include "model_test_context.h"
#include "engine_handle.h"  // white-box: external_bridge

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

CATCH_REGISTER_LISTENER(FacadeTestListener)

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

/// @brief Case-insensitive substring test.
bool has_ci(const std::string& hay, const std::string& needle) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

/// @brief One bridge `ask` result: its text and whether it is an error.
struct Ask {
    std::string text;
    bool is_error = false;
};

/// @brief Call `entropic.ask` through the bridge, in session "s".
Ask ask(entropic_handle_t h, const std::string& tier, const std::string& prompt) {
    const json req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", {{"name", "entropic.ask"},
                               {"arguments", {{"prompt", prompt},
                                              {"session", "s"},
                                              {"tier", tier}}}}}};
    const auto reply = json::parse(h->external_bridge->dispatch(req.dump(), -1));
    const auto result = reply.value("result", json::object());
    Ask out;
    out.is_error = result.value("isError", false);
    for (const auto& c : result.value("content", json::array())) {
        out.text += c.value("text", "");
    }
    return out;
}

}  // namespace

SCENARIO("v2.14.5: a failed turn is an error carrying only its own text",
         "[model][v2145][bridge][gh209][gh212]") {
    GIVEN("a served engine with a free tier and a tier that cannot complete") {
        auto gguf = entropic::test::facade::model_gguf("gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }
        entropic::test::facade::FacadeProject project("v2145_failed_turn");
        project.extra_mcp_config = "  external:\n    enabled: true\n";
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body = "You are a terse assistant with a good memory.";
        lead.context_length = 4096;
        lead.allowed_tools = {"entropic.complete"};
        lead.explicit_completion = false;
        entropic::test::facade::TierSpec strict = lead;
        strict.name = "strict";
        strict.identity_body = "You are a terse assistant. Answer in plain prose.";
        strict.allowed_tools = {};
        strict.no_tools = true;
        strict.explicit_completion = true;
        strict.max_consecutive_empty_turns = 1;
        auto* h = project.setup({lead, strict});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);
        REQUIRE(h->external_bridge != nullptr);

        const auto first = ask(h, "lead",
            "Remember the word Lighthouse. What is the capital of France? One word.");
        const auto failed = ask(h, "strict", "Name one primary colour.");
        const auto third = ask(h, "lead",
            "What word did I ask you to remember? One word.");
        INFO("first: " << first.text << "\nfailed: " << failed.text
             << "\nthird: " << third.text);

        // ONE THEN: Catch2 re-runs the whole GIVEN per THEN.
        THEN("the failed ask is an error with its reason and only its own text") {
            CHECK_FALSE(first.is_error);
            CHECK(has_ci(first.text, "paris"));

            CHECK(failed.is_error);
            CHECK(failed.text.rfind("error: turn failed", 0) == 0);
            CHECK(failed.text.find("zero_tool_calls_with_explicit_completion")
                  != std::string::npos);
            CHECK(failed.text.find("strict") != std::string::npos);
            CHECK_FALSE(has_ci(failed.text, "paris"));  // gh#209
            CHECK(failed.text.find("\"action\"") == std::string::npos);  // gh#212
            // Engagement: the model did answer in prose, and that text was
            // offered as unconfirmed rather than dropped.
            CHECK(failed.text.find("Unconfirmed text from this turn")
                  != std::string::npos);

            CHECK_FALSE(third.is_error);
            CHECK(has_ci(third.text, "lighthouse"));
        }
    }
}
