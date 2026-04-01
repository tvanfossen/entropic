/**
 * @file test_constitutional_validator.cpp
 * @brief ConstitutionalValidator unit tests — critique, parsing, revision.
 * @version 1.9.8
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
    const char* /*messages_json*/,
    const char* /*params_json*/,
    char** result_json,
    void* user_data) {
    auto* mock = static_cast<MockValidationInference*>(user_data);
    mock->generate_count++;

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
