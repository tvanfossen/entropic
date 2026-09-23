// SPDX-License-Identifier: Apache-2.0
/**
 * @file tier_grammar_gate_test.cpp
 * @brief A tier grammar that is not registered fails LOUD at first use
 *        (gh#154 follow-up, v2.13.0).
 *
 * gh#154 first closed the fail-open at CONFIGURE: a tier whose `grammar:`
 * stem resolved to no `.gbnf` returned `ENTROPIC_ERROR_INVALID_CONFIG`.
 * That gate broke a documented C API workflow. `entropic_grammar_register`
 * and `entropic_grammar_register_file` both require an ORCHESTRATOR
 * (`check_orchestrator`), which exists only AFTER `entropic_configure*` —
 * so "configure, then register the tier's grammar" was the only sequence
 * available to a consumer holding its grammar in memory or at a path the
 * engine cannot discover, and a configure-time refusal made it impossible.
 * `tests/model/test_gh95_identity_grammar.cpp` is that exact sequence and
 * stopped passing.
 *
 * So the refusal moves to FIRST USE. Configure warns and continues; a run
 * that SELECTS the tier while its grammar is still unregistered fails with
 * `ENTROPIC_ERROR_GRAMMAR_NOT_FOUND` and decodes nothing. Never silently
 * unconstrained, and never impossible to register.
 *
 * The check runs BEFORE `get_model`, which is both the cheaper order (no
 * model swap for a doomed run) and what makes it provable here: these
 * tests need no GGUF at all. The tier is backed by a placeholder file, so
 * `initialize()` fails at activation — which means the registry is empty
 * (`load_bundled_grammars()` never runs) and `get_model()` returns null.
 * A refusal therefore reads GRAMMAR_NOT_FOUND, and once the grammar IS
 * registered the very same call falls through to the no-model error: the
 * gate opened.
 *
 * @version 2.13.0 (gh#154)
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/inference/orchestrator.h>
#include <entropic/types/config.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr const char* FORCE_HELLO = "root ::= \"HELLO\"\n";

/// @brief A non-GGUF file that exists — create_tier_backends accepts it,
///        activation fails, so no model is ever resident.
std::filesystem::path make_placeholder_gguf(const std::string& stem) {
    auto p = std::filesystem::temp_directory_path()
        / ("entropic_gh154_gate_" + stem + ".gguf");
    std::ofstream out(p, std::ios::binary);
    out << "NOT_A_REAL_GGUF_FILE_PLACEHOLDER";
    return p;
}

/// @brief One tier, naming `grammar` by bare stem (the spelling that
///        reaches TierConfig from identity frontmatter).
entropic::ParsedConfig make_tier_grammar_config(
    const std::filesystem::path& fake_gguf,
    const std::string& grammar_stem) {
    entropic::ParsedConfig config;
    config.models.default_tier = "lead";
    config.vram_reserve_mb = 0;

    entropic::TierConfig lead;
    lead.path = fake_gguf;
    lead.adapter = "generic";
    lead.context_length = 1024;
    if (!grammar_stem.empty()) {
        lead.grammar = std::filesystem::path(grammar_stem);
    }
    config.models.tiers["lead"] = lead;
    return config;
}

std::vector<entropic::Message> one_turn() {
    return {{"user", "Write a short paragraph about cats.", {}, {}}};
}

/// @brief RAII placeholder GGUF — removed even when an assertion throws.
struct Placeholder {
    std::filesystem::path path;
    explicit Placeholder(const std::string& stem)
        : path(make_placeholder_gguf(stem)) {}
    ~Placeholder() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    Placeholder(const Placeholder&) = delete;
    Placeholder& operator=(const Placeholder&) = delete;
};

}  // namespace

SCENARIO("gh#154 a tier grammar that is not registered refuses the run",
         "[gh154][v2.13.0][inference][orchestrator][grammar][failure-mode]")
{
    Placeholder fake("refuse");
    entropic::ModelOrchestrator orch;
    // initialize() fails at activation (placeholder GGUF) but assigns
    // config_ first, so the tier and its grammar stem are populated and
    // the grammar registry is empty — exactly the consumer's state
    // between entropic_configure_dir and entropic_grammar_register_file.
    REQUIRE_FALSE(orch.initialize(
        make_tier_grammar_config(fake.path, "forcehello")));
    REQUIRE(orch.grammar_registry().get("forcehello").empty());

    const auto msgs = one_turn();
    entropic::GenerationParams params;
    params.max_tokens = 1;

    GIVEN("the tier's grammar has not been registered yet") {
        WHEN("a run selects that tier") {
            auto result = orch.generate(msgs, params, "lead");

            THEN("it fails with GRAMMAR_NOT_FOUND, not a silent "
                 "unconstrained decode") {
                INFO("error_message: " << result.error_message);
                CHECK(result.error_code
                      == ENTROPIC_ERROR_GRAMMAR_NOT_FOUND);
                CHECK(result.finish_reason == "error");
            }
            AND_THEN("nothing was decoded") {
                CHECK(result.content.empty());
                CHECK(result.token_count == 0);
            }
            AND_THEN("the message names the tier, the stem, and the call "
                     "that fixes it") {
                const auto& err = result.error_message;
                INFO("error_message: " << err);
                CHECK(err.find("lead") != std::string::npos);
                CHECK(err.find("forcehello") != std::string::npos);
                CHECK(err.find("entropic_grammar_register")
                      != std::string::npos);
            }
        }

        WHEN("the cancellable overload selects that tier") {
            std::atomic<bool> cancel{false};
            auto result = orch.generate(msgs, params, cancel, "lead");

            THEN("it refuses identically — one rule, every entry point") {
                CHECK(result.error_code
                      == ENTROPIC_ERROR_GRAMMAR_NOT_FOUND);
                CHECK(result.content.empty());
            }
        }

        WHEN("the streaming path selects that tier") {
            std::atomic<bool> cancel{false};
            int token_callbacks = 0;
            auto result = orch.generate_streaming(
                msgs, params,
                [&token_callbacks](std::string_view) { ++token_callbacks; },
                cancel, "lead");

            THEN("it refuses and never emits a token") {
                CHECK(result.error_code
                      == ENTROPIC_ERROR_GRAMMAR_NOT_FOUND);
                CHECK(token_callbacks == 0);
            }
        }

        WHEN("a batch fans out over that tier") {
            std::atomic<bool> cancel{false};
            std::vector<std::vector<entropic::Message>> batch{msgs, msgs};
            std::vector<entropic::GenerationParams> batch_params(2, params);
            std::vector<std::string> tiers{"lead", "lead"};
            auto results = orch.generate_batch(
                batch, batch_params, tiers, cancel);

            THEN("every arm carries the refusal — a shared decode cannot "
                 "run half-constrained") {
                REQUIRE(results.size() == 2);
                for (const auto& r : results) {
                    CHECK(r.error_code == ENTROPIC_ERROR_GRAMMAR_NOT_FOUND);
                    CHECK(r.content.empty());
                }
            }
        }
    }
}

SCENARIO("gh#154 registering the tier's grammar after configure opens the "
         "gate",
         "[gh154][v2.13.0][inference][orchestrator][grammar]")
{
    Placeholder fake("register");
    entropic::ModelOrchestrator orch;
    REQUIRE_FALSE(orch.initialize(
        make_tier_grammar_config(fake.path, "forcehello")));

    const auto msgs = one_turn();
    entropic::GenerationParams params;
    params.max_tokens = 1;

    GIVEN("a run that was refused for the unregistered grammar") {
        REQUIRE(orch.generate(msgs, params, "lead").error_code
                == ENTROPIC_ERROR_GRAMMAR_NOT_FOUND);

        WHEN("the grammar is registered the way a consumer registers it "
             "— after configure, through the registry the C API writes to") {
            REQUIRE(orch.grammar_registry().register_grammar(
                "forcehello", FORCE_HELLO));

            THEN("the grammar gate no longer refuses the tier") {
                auto result = orch.generate(msgs, params, "lead");
                INFO("error_message: " << result.error_message);
                // The run still fails — there is no loadable model here —
                // but NOT on the grammar. That distinction is the whole
                // point: the gate opened.
                CHECK(result.error_code
                      != ENTROPIC_ERROR_GRAMMAR_NOT_FOUND);
            }
        }
    }
}

SCENARIO("gh#154 a REQUEST-level grammar_key miss still fails open",
         "[gh154][v2.13.0][inference][orchestrator][grammar]")
{
    // The deliberate asymmetry. A tier stem is static config and the tier
    // is selected by the engine, so a miss there is a broken deployment.
    // `params.grammar_key` is per-call and may name a grammar the caller
    // registers later; it keeps the documented fail-open and reports
    // itself through `generations[].grammar.resolved == false`.
    Placeholder fake("request_key");
    entropic::ModelOrchestrator orch;
    REQUIRE_FALSE(orch.initialize(
        make_tier_grammar_config(fake.path, /*grammar_stem=*/"")));

    entropic::GenerationParams params;
    params.max_tokens = 1;
    params.grammar_key = "never-registered";

    GIVEN("a per-call grammar_key that resolves to nothing") {
        WHEN("the run dispatches") {
            auto result = orch.generate(one_turn(), params, "lead");

            THEN("it is NOT refused by the tier-grammar gate") {
                INFO("error_message: " << result.error_message);
                CHECK(result.error_code
                      != ENTROPIC_ERROR_GRAMMAR_NOT_FOUND);
            }
        }
    }
}
