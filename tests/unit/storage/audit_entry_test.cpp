/**
 * @file test_audit_entry.cpp
 * @brief BDD tests for AuditEntry JSON serialization.
 * @version 1.9.5
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <entropic/storage/audit_entry.h>

#include <nlohmann/json.hpp>

#include <string>

/**
 * @brief Create a fully-populated test entry.
 * @return AuditEntry with all fields set.
 * @internal
 * @version 1.9.5
 */
static entropic::AuditEntry make_test_entry() {
    entropic::AuditEntry e;
    e.caller_id = "eng";
    e.tool_name = "filesystem.write_file";
    e.params_json = R"({"path":"/src/main.cpp","content":"hello"})";
    e.result_status = "success";
    e.result_content = "Wrote 5 bytes to /src/main.cpp";
    e.elapsed_ms = 8.3;
    e.directives_json = R"([{"type":"stop_processing"}])";
    e.delegation_depth = 1;
    e.iteration = 5;
    e.parent_conversation_id = "a1b2c3d4-parent";
    return e;
}

SCENARIO("AuditEntry serializes all fields to JSON",
         "[storage][audit]") {
    GIVEN("A fully-populated AuditEntry") {
        auto entry = make_test_entry();

        WHEN("serialized to JSON") {
            auto j = entropic::audit_entry_to_json(entry);

            THEN("all fields are present and correct") {
                REQUIRE(j["caller_id"] == "eng");
                REQUIRE(j["tool_name"] == "filesystem.write_file");
                REQUIRE(j["params"]["path"] == "/src/main.cpp");
                REQUIRE(j["result"]["status"] == "success");
                REQUIRE(j["result"]["content"] ==
                        "Wrote 5 bytes to /src/main.cpp");
                REQUIRE(j["result"]["elapsed_ms"] ==
                        Catch::Approx(8.3));
                REQUIRE(j["directives"].size() == 1);
                REQUIRE(j["delegation_depth"] == 1);
                REQUIRE(j["iteration"] == 5);
                REQUIRE(j["parent_conversation_id"] ==
                        "a1b2c3d4-parent");
            }
        }
    }
}

SCENARIO("AuditEntry serializes null parent_conversation_id",
         "[storage][audit]") {
    GIVEN("An entry with empty parent_conversation_id") {
        auto entry = make_test_entry();
        entry.parent_conversation_id = "";

        WHEN("serialized to JSON") {
            auto j = entropic::audit_entry_to_json(entry);

            THEN("parent_conversation_id is null") {
                REQUIRE(j["parent_conversation_id"].is_null());
            }
        }
    }
}

SCENARIO("AuditEntry round-trips through JSON",
         "[storage][audit]") {
    GIVEN("A serialized AuditEntry") {
        auto original = make_test_entry();
        auto j = entropic::audit_entry_to_json(original);

        WHEN("deserialized back") {
            entropic::AuditEntry parsed;
            bool ok = entropic::audit_entry_from_json(j, parsed);

            THEN("all fields match the original") {
                REQUIRE(ok);
                REQUIRE(parsed.caller_id == original.caller_id);
                REQUIRE(parsed.tool_name == original.tool_name);
                REQUIRE(parsed.result_status ==
                        original.result_status);
                REQUIRE(parsed.result_content ==
                        original.result_content);
                REQUIRE(parsed.elapsed_ms ==
                        Catch::Approx(original.elapsed_ms));
                REQUIRE(parsed.delegation_depth ==
                        original.delegation_depth);
                REQUIRE(parsed.iteration == original.iteration);
                REQUIRE(parsed.parent_conversation_id ==
                        original.parent_conversation_id);
            }
        }
    }
}

SCENARIO("AuditEntry from_json fails on missing required field",
         "[storage][audit]") {
    GIVEN("A JSON object missing caller_id") {
        nlohmann::json j;
        j["tool_name"] = "bash.execute";

        WHEN("deserialized") {
            entropic::AuditEntry entry;
            bool ok = entropic::audit_entry_from_json(j, entry);

            THEN("it returns false") {
                REQUIRE_FALSE(ok);
            }
        }
    }
}

SCENARIO("AuditEntry from_json ignores extra fields",
         "[storage][audit]") {
    GIVEN("A JSON object with extra unknown fields") {
        auto entry = make_test_entry();
        auto j = entropic::audit_entry_to_json(entry);
        j["version"] = 1;
        j["timestamp"] = "2026-03-18T14:32:01.847Z";
        j["session_id"] = "some-uuid";
        j["sequence"] = 42;
        j["unknown_field"] = "ignored";

        WHEN("deserialized") {
            entropic::AuditEntry parsed;
            bool ok = entropic::audit_entry_from_json(j, parsed);

            THEN("it succeeds and matches original fields") {
                REQUIRE(ok);
                REQUIRE(parsed.tool_name == entry.tool_name);
            }
        }
    }
}

SCENARIO("AuditEntry preserves large params without truncation",
         "[storage][audit]") {
    GIVEN("An entry with 10KB params") {
        auto entry = make_test_entry();
        std::string big_value(10240, 'X');
        entry.params_json = R"({"data":")" + big_value + R"("})";

        WHEN("round-tripped through JSON") {
            auto j = entropic::audit_entry_to_json(entry);
            entropic::AuditEntry parsed;
            entropic::audit_entry_from_json(j, parsed);

            THEN("params are not truncated") {
                auto parsed_params = nlohmann::json::parse(
                    parsed.params_json);
                REQUIRE(parsed_params["data"].get<std::string>()
                        .size() == 10240);
            }
        }
    }
}

SCENARIO("AuditEntry preserves large result content",
         "[storage][audit]") {
    GIVEN("An entry with large result content") {
        auto entry = make_test_entry();
        entry.result_content = std::string(10240, 'Y');

        WHEN("round-tripped through JSON") {
            auto j = entropic::audit_entry_to_json(entry);
            entropic::AuditEntry parsed;
            entropic::audit_entry_from_json(j, parsed);

            THEN("result content is not truncated") {
                REQUIRE(parsed.result_content.size() == 10240);
            }
        }
    }
}

SCENARIO("AuditEntry serializes directive array correctly",
         "[storage][audit]") {
    GIVEN("An entry with 3 directives") {
        auto entry = make_test_entry();
        entry.directives_json =
            R"([{"type":"delegate","target":"eng"},)"
            R"({"type":"stop_processing"},)"
            R"({"type":"complete"}])";

        WHEN("serialized") {
            auto j = entropic::audit_entry_to_json(entry);

            THEN("directives array has 3 elements") {
                REQUIRE(j["directives"].size() == 3);
                REQUIRE(j["directives"][0]["type"] == "delegate");
            }
        }
    }
}

SCENARIO("AuditEntry serializes empty directives as empty array",
         "[storage][audit]") {
    GIVEN("An entry with empty directives") {
        auto entry = make_test_entry();
        entry.directives_json = "[]";

        WHEN("serialized") {
            auto j = entropic::audit_entry_to_json(entry);

            THEN("directives is an empty array") {
                REQUIRE(j["directives"].is_array());
                REQUIRE(j["directives"].empty());
            }
        }
    }
}
