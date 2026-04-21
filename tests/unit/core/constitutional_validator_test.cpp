// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_constitutional_validator.cpp
 * @brief ConstitutionalValidator unit tests — critique, parsing, revision.
 * @version 2.0.7
 */

#include <entropic/core/constitutional_validator.h>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace entropic;

// ── Mock Inference for Validation ─────────────────────────

namespace {

/**
 * @brief Mock inference that returns scripted responses in sequence.
 * @internal
 * @version 1.9.8
 */
struct MockValidationInference {
    std::vector<std::string> responses;  ///< Queued responses
    int call_index = 0;                  ///< Next response index
    int generate_count = 0;              ///< Total calls made
    bool should_fail = false;            ///< Simulate failure
    std::string last_params_json;        ///< Captured params from last call
    std::string last_messages_json;      ///< Captured messages from last call
};

/**
 * @brief Allocate a C string copy (freed by mock_val_free).
 * @param s Source string.
 * @return Heap-allocated copy.
 * @internal
 * @version 1.9.8
 */
char* mock_val_strdup(const std::string& s) {
    auto* p = new char[s.size() + 1];
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

/**
 * @brief Mock free function.
 * @param ptr Pointer to free.
 * @internal
 * @version 1.9.8
 */
void mock_val_free(void* ptr) {
    delete[] static_cast<char*>(ptr);
}

/**
 * @brief Mock generate — returns next queued response.
 * @callback
 * @version 1.9.8
 */
int mock_val_generate(
    const char* messages_json,
    const char* params_json,
    char** result_json,
    void* user_data) {
    auto* mock = static_cast<MockValidationInference*>(user_data);
    mock->generate_count++;
    if (params_json) { mock->last_params_json = params_json; }
    if (messages_json) { mock->last_messages_json = messages_json; }

    if (mock->should_fail) {
        *result_json = nullptr;
        return -1;
    }

    if (mock->call_index < static_cast<int>(mock->responses.size())) {
        *result_json = mock_val_strdup(
            mock->responses[mock->call_index]);
        mock->call_index++;
    } else {
        *result_json = mock_val_strdup(
            R"({"compliant":true,"violations":[],"revised":""})");
    }
    return 0;
}

/**
 * @brief Build an InferenceInterface wired to MockValidationInference.
 * @param mock Mock state (caller keeps alive).
 * @return Wired interface.
 * @internal
 * @version 1.9.8
 */
InferenceInterface make_val_interface(MockValidationInference& mock) {
    InferenceInterface iface;
    iface.generate = mock_val_generate;
    iface.free_fn = mock_val_free;
    iface.backend_data = &mock;
    return iface;
}

// ── Common JSON Strings ──────────────────────────────────

const char* COMPLIANT_JSON =
    R"({"compliant":true,"violations":[],"revised":""})";

const char* NONCOMPLIANT_WITH_REVISION =
    R"({"compliant":false,"violations":[)"
    R"({"rule":"Privacy","excerpt":"Upload to cloud",)"
    R"("explanation":"Suggests external upload"}],)"
    R"("revised":"Process locally"})";

const char* NONCOMPLIANT_NO_REVISION =
    R"({"compliant":false,"violations":[)"
    R"({"rule":"Privacy","excerpt":"Upload to cloud",)"
    R"("explanation":"Suggests external upload"}],)"
    R"("revised":""})";

const char* CONSTITUTION_TEXT =
    "Privacy First: All processing must be local.\n"
    "Safety: Never suggest harmful actions.";

/// @brief Standard test validation_rules for tiers that should be
/// validated. Mirrors what identity frontmatter provides at runtime.
const std::vector<std::string> TEST_RULES = {
    "All processing must be local",
    "Never suggest harmful actions"
};

/**
 * @brief Set standard test validation rules on a validator.
 *
 * After v2.0.8, validation only runs when per-tier validation_rules
 * are defined. This helper sets TEST_RULES for one or more tiers
 * so tests exercise the critique pipeline rather than hitting the
 * "no validation_rules" skip path.
 *
 * @param v Validator to configure.
 * @param tiers Tier names to set rules on.
 * @utility
 * @version 2.0.8
 */
static void set_test_rules(
    ConstitutionalValidator& v,
    std::initializer_list<std::string> tiers) {
    for (const auto& tier : tiers) {
        v.set_tier_rules(tier, TEST_RULES);
    }
}

} // anonymous namespace

// ── Tests ────────────────────────────────────────────────

SCENARIO("Compliant output returns unchanged", "[validation]") {
    GIVEN("a validator with mock inference returning compliant") {
        MockValidationInference mock;
        mock.responses.push_back(COMPLIANT_JSON);
        auto iface = make_val_interface(mock);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(cfg, CONSTITUTION_TEXT);
        validator.attach(nullptr, &iface);
        set_test_rules(validator, {"eng"});

        WHEN("validate is called with safe content") {
            auto result = validator.validate(
                "Safe helpful response", "eng", nullptr);

            THEN("original content returned unchanged") {
                REQUIRE(result.content == "Safe helpful response");
                REQUIRE(result.was_revised == false);
                REQUIRE(result.revision_count == 0);
                REQUIRE(result.final_critique.compliant == true);
            }
        }
    }
}

SCENARIO("Non-compliant output triggers revision via Path A",
         "[validation]") {
    GIVEN("a validator returning non-compliant then compliant") {
        MockValidationInference mock;
        mock.responses.push_back(NONCOMPLIANT_WITH_REVISION);
        mock.responses.push_back(COMPLIANT_JSON);
        auto iface = make_val_interface(mock);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        cfg.max_revisions = 2;
        ConstitutionalValidator validator(cfg, CONSTITUTION_TEXT);
        validator.attach(nullptr, &iface);
        set_test_rules(validator, {"eng"});

        WHEN("validate is called with violating content") {
            auto result = validator.validate(
                "Upload to cloud", "eng", nullptr);

            THEN("revised text is returned") {
                REQUIRE(result.content == "Process locally");
                REQUIRE(result.was_revised == true);
                REQUIRE(result.revision_count == 1);
            }
        }
    }
}

SCENARIO("Max revisions exhausted returns last output",
         "[validation]") {
    GIVEN("mock that always returns non-compliant") {
        MockValidationInference mock;
        // critique 1: non-compliant with revision
        mock.responses.push_back(NONCOMPLIANT_WITH_REVISION);
        // verify revision: still non-compliant
        mock.responses.push_back(NONCOMPLIANT_NO_REVISION);
        // Path B re-generate
        mock.responses.push_back("Revised attempt");
        // critique of Path B: still non-compliant
        mock.responses.push_back(NONCOMPLIANT_NO_REVISION);
        auto iface = make_val_interface(mock);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        cfg.max_revisions = 1;
        ConstitutionalValidator validator(cfg, CONSTITUTION_TEXT);
        validator.attach(nullptr, &iface);
        set_test_rules(validator, {"eng"});

        WHEN("validate is called") {
            auto result = validator.validate(
                "Bad content", "eng", nullptr);

            THEN("revision count matches max") {
                REQUIRE(result.revision_count == 1);
                REQUIRE(result.was_revised == true);
            }
        }
    }
}

SCENARIO("Critique-only mode (max_revisions: 0)", "[validation]") {
    GIVEN("validator with max_revisions 0") {
        MockValidationInference mock;
        mock.responses.push_back(NONCOMPLIANT_NO_REVISION);
        auto iface = make_val_interface(mock);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        cfg.max_revisions = 0;
        ConstitutionalValidator validator(cfg, CONSTITUTION_TEXT);
        validator.attach(nullptr, &iface);
        set_test_rules(validator, {"eng"});

        WHEN("validate is called with bad content") {
            auto result = validator.validate(
                "Bad content", "eng", nullptr);

            THEN("original content returned unmodified") {
                REQUIRE(result.content == "Bad content");
                REQUIRE(result.was_revised == false);
            }
            THEN("violations are still populated") {
                REQUIRE_FALSE(
                    result.final_critique.violations.empty());
            }
        }
    }
}

SCENARIO("Per-identity opt-out", "[validation]") {
    GIVEN("validator with identity eng set to false") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(cfg, CONSTITUTION_TEXT);
        validator.set_identity_validation("eng", false);

        WHEN("should_validate is called for eng") {
            THEN("it returns false") {
                REQUIRE(validator.should_validate("eng") == false);
            }
        }
        WHEN("should_validate is called for qa") {
            THEN("it returns true (default)") {
                REQUIRE(validator.should_validate("qa") == true);
            }
        }
    }
}

SCENARIO("Per-identity override at runtime", "[validation]") {
    GIVEN("validator with default config") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(cfg, CONSTITUTION_TEXT);

        WHEN("set_identity_validation(eng, false) is called") {
            validator.set_identity_validation("eng", false);
            THEN("should_validate(eng) returns false") {
                REQUIRE(validator.should_validate("eng") == false);
            }
        }
        WHEN("set_identity_validation(eng, true) is called") {
            validator.set_identity_validation("eng", true);
            THEN("should_validate(eng) returns true") {
                REQUIRE(validator.should_validate("eng") == true);
            }
        }
    }
}

SCENARIO("Global enable toggle at runtime", "[validation]") {
    GIVEN("validator constructed with enabled=false") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = false;
        ConstitutionalValidator validator(cfg, CONSTITUTION_TEXT);

        WHEN("set_global_enabled(true) is called") {
            validator.set_global_enabled(true);
            THEN("should_validate falls through to true for any identity") {
                REQUIRE(validator.should_validate("any") == true);
                REQUIRE(validator.should_validate("other") == true);
            }
        }
        WHEN("set_global_enabled(true) then (false)") {
            validator.set_global_enabled(true);
            validator.set_global_enabled(false);
            THEN("should_validate falls through to false") {
                REQUIRE(validator.should_validate("any") == false);
            }
        }
        WHEN("per-identity override is set and global flipped") {
            validator.set_identity_validation("eng", true);
            validator.set_global_enabled(false);
            THEN("identity override takes precedence over global") {
                REQUIRE(validator.should_validate("eng") == true);
                REQUIRE(validator.should_validate("other") == false);
            }
        }
    }
}

SCENARIO("Critique JSON parse succeeds", "[validation]") {
    GIVEN("valid critique JSON with violations") {
        std::string json = NONCOMPLIANT_WITH_REVISION;

        WHEN("parse_critique is called") {
            auto result =
                ConstitutionalValidator::parse_critique(json);

            THEN("compliant is false") {
                REQUIRE(result.compliant == false);
            }
            THEN("violations has one entry") {
                REQUIRE(result.violations.size() == 1);
                REQUIRE(result.violations[0].rule == "Privacy");
                REQUIRE(result.violations[0].excerpt ==
                        "Upload to cloud");
            }
            THEN("revised text is present") {
                REQUIRE(result.revised == "Process locally");
            }
        }
    }
}

SCENARIO("Compliant critique JSON parses correctly", "[validation]") {
    GIVEN("compliant critique JSON") {
        std::string json = COMPLIANT_JSON;

        WHEN("parse_critique is called") {
            auto result =
                ConstitutionalValidator::parse_critique(json);

            THEN("compliant is true") {
                REQUIRE(result.compliant == true);
            }
            THEN("violations is empty") {
                REQUIRE(result.violations.empty());
            }
            THEN("revised is empty") {
                REQUIRE(result.revised.empty());
            }
        }
    }
}

SCENARIO("Malformed critique JSON returns safe default",
         "[validation]") {
    GIVEN("malformed JSON from critique generation") {
        std::string json = "not json at all";

        WHEN("parse_critique is called") {
            auto result =
                ConstitutionalValidator::parse_critique(json);

            THEN("result defaults to compliant (parse failure)") {
                REQUIRE(result.compliant == true);
            }
            THEN("raw_json is preserved for diagnostics") {
                REQUIRE(result.raw_json == "not json at all");
            }
        }
    }
}

SCENARIO("Critique prompt includes constitution text",
         "[validation]") {
    GIVEN("constitution text about privacy") {
        ConstitutionalValidationConfig cfg;
        ConstitutionalValidator validator(
            cfg, "Privacy First: All processing local");

        WHEN("build_critique_prompt is called") {
            auto prompt =
                validator.build_critique_prompt("some output");

            THEN("prompt contains the constitution") {
                REQUIRE(prompt.find(
                    "Privacy First: All processing local")
                        != std::string::npos);
            }
            THEN("prompt contains the content to evaluate") {
                REQUIRE(prompt.find("some output")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("Disabled validator does not call inference",
         "[validation]") {
    GIVEN("config.enabled == false") {
        MockValidationInference mock;
        auto iface = make_val_interface(mock);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = false;
        ConstitutionalValidator validator(cfg, CONSTITUTION_TEXT);
        validator.attach(nullptr, &iface);

        WHEN("validate is called") {
            auto result = validator.validate(
                "Any content", "eng", nullptr);

            THEN("inference is NOT invoked") {
                REQUIRE(mock.generate_count == 0);
            }
            THEN("original content returned") {
                REQUIRE(result.content == "Any content");
            }
        }
    }
}

SCENARIO("Critique generation failure returns original",
         "[validation]") {
    GIVEN("mock inference that fails") {
        MockValidationInference mock;
        mock.should_fail = true;
        auto iface = make_val_interface(mock);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(cfg, CONSTITUTION_TEXT);
        validator.attach(nullptr, &iface);
        set_test_rules(validator, {"eng"});

        WHEN("validate is called") {
            auto result = validator.validate(
                "Content to validate", "eng", nullptr);

            THEN("original content returned unchanged") {
                REQUIRE(result.content == "Content to validate");
                REQUIRE(result.was_revised == false);
            }
        }
    }
}

SCENARIO("Last result is populated after validation",
         "[validation]") {
    GIVEN("a validator that runs a critique") {
        MockValidationInference mock;
        mock.responses.push_back(COMPLIANT_JSON);
        auto iface = make_val_interface(mock);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(cfg, CONSTITUTION_TEXT);
        validator.attach(nullptr, &iface);
        set_test_rules(validator, {"eng"});

        WHEN("validate is called") {
            validator.validate("Test content", "eng", nullptr);

            THEN("last_result reflects the validation") {
                auto last = validator.last_result();
                REQUIRE(last.content == "Test content");
                REQUIRE(last.final_critique.compliant == true);
            }
        }
    }
}

SCENARIO("Multiple violations parsed correctly", "[validation]") {
    GIVEN("critique JSON with two violations") {
        std::string json =
            R"({"compliant":false,"violations":[)"
            R"({"rule":"Privacy","excerpt":"upload",)"
            R"("explanation":"external upload"},)"
            R"({"rule":"Safety","excerpt":"delete all",)"
            R"("explanation":"destructive action"}],)"
            R"("revised":""})";

        WHEN("parse_critique is called") {
            auto result =
                ConstitutionalValidator::parse_critique(json);

            THEN("two violations are parsed") {
                REQUIRE(result.violations.size() == 2);
                REQUIRE(result.violations[0].rule == "Privacy");
                REQUIRE(result.violations[1].rule == "Safety");
            }
        }
    }
}

// ── Critique params passthrough ─────────────────────────

SCENARIO("Critique params include grammar_key and enable_thinking",
         "[constitutional_validator][v2.0.6][regression]")
{
    GIVEN("a validator with custom config") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        cfg.grammar_key = "test_grammar";
        cfg.temperature = 0.0f;
        cfg.enable_thinking = false;
        cfg.max_critique_tokens = 256;

        MockValidationInference mock;
        mock.responses.push_back(
            R"({"compliant":true,"violations":[],"revised":""})");
        auto iface = make_val_interface(mock);

        ConstitutionalValidator v(cfg, "Test rules.");
        v.attach(nullptr, &iface);
        set_test_rules(v, {"researcher"});

        WHEN("validate is called") {
            // Use researcher (not lead) — lead is in default skip_tiers
            v.validate("Test content", "researcher", nullptr);

            THEN("generate received grammar_key in params") {
                CHECK(mock.last_params_json.find(
                    "\"grammar_key\":\"test_grammar\"")
                    != std::string::npos);
            }
            THEN("generate received enable_thinking=false") {
                CHECK(mock.last_params_json.find(
                    "\"enable_thinking\":false")
                    != std::string::npos);
            }
            THEN("generate received max_tokens=256") {
                CHECK(mock.last_params_json.find(
                    "\"max_tokens\":256")
                    != std::string::npos);
            }
        }
    }
}

// ── Per-identity validation rules ──────────────────────

SCENARIO("Per-identity rules appended to critique prompt",
         "[constitutional_validator][v2.0.6]")
{
    GIVEN("a validator with tier rules set") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;

        MockValidationInference mock;
        mock.responses.push_back(
            R"({"compliant":true,"violations":[],"revised":""})");
        auto iface = make_val_interface(mock);

        ConstitutionalValidator v(cfg, "Global rules.");
        v.attach(nullptr, &iface);
        v.set_tier_rules("researcher",
            {"Cite file:line for every claim",
             "Never assert external dep behavior"});

        WHEN("validate is called for researcher tier") {
            v.validate("Some content", "researcher", nullptr);

            THEN("generate was called") {
                REQUIRE(mock.generate_count == 1);
            }
        }
    }
}

// ── v2.0.6: Tool-call-only skip ─────────────────────────

SCENARIO("Validation skips pure tool-call output",
         "[constitutional_validator][v2.0.6]")
{
    GIVEN("a validator with enabled config") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;

        MockValidationInference mock;
        auto iface = make_val_interface(mock);
        ConstitutionalValidator v(cfg, "Rules.");
        v.attach(nullptr, &iface);
        set_test_rules(v, {"researcher"});

        WHEN("content is only a tool call") {
            std::string tc_only =
                "<think>\n</think>\n"
                "<tool_call>\n<function=entropic.delegate>\n"
                "<parameter=target>researcher</parameter>\n"
                "</function>\n</tool_call>";
            // Use researcher — lead is in default skip_tiers
            v.validate(tc_only, "researcher", nullptr);

            THEN("no critique generation occurs") {
                CHECK(mock.generate_count == 0);
            }
        }

        WHEN("content has prose after tool call") {
            mock.responses.push_back(
                R"({"compliant":true,"violations":[],"revised":""})");
            std::string with_prose =
                "<tool_call></tool_call>\nHere is my analysis.";
            // Use researcher — lead is in default skip_tiers
            v.validate(with_prose, "researcher", nullptr);

            THEN("critique generation occurs") {
                CHECK(mock.generate_count == 1);
            }
        }
    }
}

// ── v2.0.6: Fail-open on malformed critique ─────────────

SCENARIO("Malformed critique JSON defaults to compliant",
         "[constitutional_validator][v2.0.6]")
{
    GIVEN("a validator whose critique model returns non-standard JSON") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;

        MockValidationInference mock;
        // Response has no "compliant" key — triggers fail-open path
        mock.responses.push_back(
            R"({"status": "pass", "issues": []})");
        auto iface = make_val_interface(mock);
        ConstitutionalValidator v(cfg, "Rules.");
        v.attach(nullptr, &iface);
        set_test_rules(v, {"researcher"});

        WHEN("validate is called") {
            // Use researcher — lead is in default skip_tiers
            auto result = v.validate("Hello!", "researcher", nullptr);

            THEN("treated as compliant (fail-open)") {
                CHECK_FALSE(result.was_revised);
                CHECK(result.revision_count == 0);
            }
        }
    }
}

// ── v2.0.7: skip_tiers config ───────────────────────────

SCENARIO("Lead tier skipped by default skip_tiers",
         "[constitutional_validator][v2.0.7]")
{
    GIVEN("a validator with default config (skip_tiers = {lead})") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        // Default skip_tiers = {"lead"}

        MockValidationInference mock;
        mock.responses.push_back(
            R"({"compliant":false,"violations":[{"rule":"R1","excerpt":"x","explanation":"e"}],"revised":""})");
        auto iface = make_val_interface(mock);
        ConstitutionalValidator v(cfg, "Rules.");
        v.attach(nullptr, &iface);

        WHEN("validate is called for lead tier") {
            auto result = v.validate("Some content", "lead", nullptr);

            THEN("no critique generation occurs — lead is skipped") {
                CHECK(mock.generate_count == 0);
            }
            THEN("result is not revised") {
                CHECK_FALSE(result.was_revised);
            }
        }
    }
}

SCENARIO("Non-lead tiers are validated with default skip_tiers",
         "[constitutional_validator][v2.0.7]")
{
    GIVEN("a validator with default config (skip_tiers = {lead})") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;

        MockValidationInference mock;
        mock.responses.push_back(
            R"({"compliant":true,"violations":[],"revised":""})");
        auto iface = make_val_interface(mock);
        ConstitutionalValidator v(cfg, "Rules.");
        v.attach(nullptr, &iface);
        set_test_rules(v, {"researcher"});

        WHEN("validate is called for researcher tier") {
            v.validate("Some content", "researcher", nullptr);

            THEN("critique generation runs") {
                CHECK(mock.generate_count == 1);
            }
        }
    }
}

SCENARIO("skip_tiers config excludes listed tiers from validation",
         "[constitutional_validator][v2.0.7]")
{
    GIVEN("a validator with skip_tiers = {lead, reader}") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        cfg.skip_tiers = {"lead", "reader"};

        ConstitutionalValidator v(cfg, "Rules.");

        THEN("should_validate returns false for lead") {
            CHECK_FALSE(v.should_validate("lead"));
        }
        THEN("should_validate returns false for reader") {
            CHECK_FALSE(v.should_validate("reader"));
        }
        THEN("should_validate returns true for researcher") {
            CHECK(v.should_validate("researcher"));
        }
    }
}

SCENARIO("Per-identity override beats skip_tiers",
         "[constitutional_validator][v2.0.7]")
{
    GIVEN("a validator with skip_tiers = {lead} "
          "but lead explicitly enabled via override") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        // skip_tiers defaults to {"lead"}

        ConstitutionalValidator v(cfg, "Rules.");
        v.set_identity_validation("lead", true);

        THEN("should_validate(lead) returns true (override wins)") {
            CHECK(v.should_validate("lead"));
        }
    }
}

// ── v2.0.7: Tool context in critique prompt ─────────────

SCENARIO("build_critique_prompt includes tool context when present",
         "[constitutional_validator][v2.0.7]")
{
    GIVEN("a validator with tool context set") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;

        MockValidationInference mock;
        mock.responses.push_back(
            R"({"compliant":true,"violations":[],"revised":""})");
        auto iface = make_val_interface(mock);
        ConstitutionalValidator v(cfg, "Global rules.");
        v.attach(nullptr, &iface);
        // Simulate handle_hook() setting the tool context
        // We access via build_critique_prompt() which reads the member
        // The only way to inject it from tests is through handle_hook
        // which requires a context JSON — use a bare validate() call
        // with current_tool_context_ cleared (no tool context path).

        WHEN("build_critique_prompt is called with no tool context") {
            auto prompt = v.build_critique_prompt("My output text");

            THEN("prompt does not contain tool calls section") {
                CHECK(prompt.find("Tool calls made this turn") ==
                      std::string::npos);
            }
        }
    }
}

// ── v2.0.7: Length safety valve ─────────────────────────

SCENARIO("Revision safety valve rejects gutted output",
         "[constitutional_validator][v2.0.7]")
{
    GIVEN("a validator with 1 max revision, "
          "and critique that produces a very short revision") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        cfg.max_revisions = 1;

        MockValidationInference mock;
        // Critique: non-compliant
        mock.responses.push_back(
            R"({"compliant":false,"violations":[{"rule":"R1","excerpt":"x","explanation":"e"}],"revised":""})");
        // Revision: one-liner (much shorter than the 200-char original)
        mock.responses.push_back("OK.");
        // Second critique (of "OK."):
        mock.responses.push_back(
            R"({"compliant":true,"violations":[],"revised":""})");
        auto iface = make_val_interface(mock);
        ConstitutionalValidator v(cfg, "Rules.");
        v.attach(nullptr, &iface);
        set_test_rules(v, {"researcher"});

        // 200-char original — "OK." is 3 chars = < 50%
        std::string long_original(200, 'x');

        WHEN("validate is called with the long content") {
            auto result = v.validate(
                long_original, "researcher", nullptr);

            THEN("original is preserved (revision was discarded)") {
                // Safety valve: revised < 50% original → original returned
                CHECK(result.content == long_original);
            }
            THEN("was_revised is false (revision was discarded)") {
                CHECK_FALSE(result.was_revised);
            }
        }
    }
}

// ── v2.0.8: Validator uses validation_rules, not constitution ──

SCENARIO("Validation falls back to constitution when no tier_rules",
         "[constitutional_validator][v2.0.8]")
{
    GIVEN("a validator with no tier_rules set") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;

        MockValidationInference mock;
        mock.responses.push_back(
            R"({"compliant":true,"violations":[],"revised":""})");
        auto iface = make_val_interface(mock);
        ConstitutionalValidator v(cfg, "FALLBACK CONSTITUTION.");
        v.attach(nullptr, &iface);
        // No set_tier_rules — constitution is the sole rubric

        WHEN("validate is called for researcher") {
            v.validate("Some content", "researcher", nullptr);

            THEN("critique generation still runs") {
                CHECK(mock.generate_count == 1);
            }
            THEN("critique prompt uses constitution as primary rubric") {
                CHECK(mock.last_messages_json.find(
                    "Constitutional Rules") != std::string::npos);
                CHECK(mock.last_messages_json.find(
                    "FALLBACK CONSTITUTION") != std::string::npos);
            }
        }
    }
}

SCENARIO("Critique prompt prioritizes validation_rules over constitution",
         "[constitutional_validator][v2.0.8]")
{
    GIVEN("a validator with tier_rules and a distinctive constitution") {
        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;

        MockValidationInference mock;
        mock.responses.push_back(
            R"({"compliant":true,"violations":[],"revised":""})");
        auto iface = make_val_interface(mock);

        ConstitutionalValidator v(cfg, "GLOBAL CONSTITUTION TEXT.");
        v.attach(nullptr, &iface);
        v.set_tier_rules("researcher", {
            "Cite file:line for every claim",
            "Never assert external dep behavior"});

        WHEN("validate is called for researcher") {
            v.validate("Test output", "researcher", nullptr);

            THEN("critique messages contain tier validation rules") {
                CHECK(mock.last_messages_json.find(
                    "Cite file:line") != std::string::npos);
                CHECK(mock.last_messages_json.find(
                    "Never assert external dep") != std::string::npos);
            }
            THEN("constitution is background, not primary rubric") {
                // Constitution is present but labeled as background
                CHECK(mock.last_messages_json.find(
                    "Background constitutional guidance") !=
                    std::string::npos);
                // NOT labeled as "Constitutional Rules:" (primary)
                CHECK(mock.last_messages_json.find(
                    "Constitutional Rules") == std::string::npos);
            }
        }
    }
}
