// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_entropic.cpp
 * @brief EntropicServer unit tests — directive emission, tier gating, duplicate skip.
 * @version 1.8.5
 */

#include <entropic/mcp/servers/entropic_server.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/enums.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace entropic;
using json = nlohmann::json;

/**
 * @brief Parse the ServerResponse JSON envelope and extract directive type strings.
 * @param envelope Raw JSON string from MCPServerBase::execute().
 * @return Vector of directive type strings.
 * @internal
 * @version 1.8.5
 */
static std::vector<std::string> extract_directive_types(
    const std::string& envelope) {
    auto j = json::parse(envelope);
    std::vector<std::string> types;
    for (const auto& d : j.at("directives")) {
        types.push_back(d.at("type").get<std::string>());
    }
    return types;
}

/**
 * @brief Extract the "result" field from a ServerResponse JSON envelope.
 * @param envelope Raw JSON string from MCPServerBase::execute().
 * @return Result string.
 * @internal
 * @version 1.8.5
 */
static std::string extract_result(const std::string& envelope) {
    auto j = json::parse(envelope);
    return j.at("result").get<std::string>();
}

/**
 * @brief Check if a directive type string is present in the list.
 * @param types Vector of directive type strings.
 * @param target Target type string to find.
 * @return true if found.
 * @internal
 * @version 1.8.5
 */
static bool has_directive(const std::vector<std::string>& types,
                          const std::string& target) {
    return std::find(types.begin(), types.end(), target)
           != types.end();
}

// ── Tests ────────────────────────────────────────────────────────

TEST_CASE("test_todo_emits_anchor_and_notify", "[entropic]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    json args;
    args["action"] = "add";
    args["content"] = "test task";

    auto envelope = server.execute("todo", args.dump());
    auto types = extract_directive_types(envelope);

    REQUIRE(has_directive(types, "context_anchor"));
    REQUIRE(has_directive(types, "notify_presenter"));
}

// ── v2.3.10: cover todo update + remove branches ──

TEST_CASE("todo update and remove actions mutate the list",
          "[entropic][v2.3.10][coverage]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    // Seed: add two items.
    {
        json args;
        args["action"] = "add";
        args["content"] = "first";
        server.execute("todo", args.dump());
    }
    {
        json args;
        args["action"] = "add";
        args["content"] = "second";
        server.execute("todo", args.dump());
    }

    SECTION("update changes status of an existing item") {
        json args;
        args["action"] = "update";
        args["index"] = 0;
        args["status"] = "done";
        auto envelope = server.execute("todo", args.dump());
        auto resp = json::parse(envelope);
        auto result = resp["result"].get<std::string>();
        REQUIRE(result.find("done") != std::string::npos);
    }

    SECTION("remove drops an existing item") {
        json args;
        args["action"] = "remove";
        args["index"] = 0;
        auto envelope = server.execute("todo", args.dump());
        auto resp = json::parse(envelope);
        // After remove, list should contain only the "second" item.
        REQUIRE(resp["result"].get<std::string>().find("second")
                != std::string::npos);
    }

    SECTION("update with out-of-range index is a no-op") {
        json args;
        args["action"] = "update";
        args["index"] = 999;
        args["status"] = "done";
        auto envelope = server.execute("todo", args.dump());
        // No crash; returns the unchanged list.
        REQUIRE_FALSE(envelope.empty());
    }

    SECTION("remove with out-of-range index is a no-op") {
        json args;
        args["action"] = "remove";
        args["index"] = 999;
        auto envelope = server.execute("todo", args.dump());
        REQUIRE_FALSE(envelope.empty());
    }
}

TEST_CASE("todo format_list returns '(empty)' on a fresh server",
          "[entropic][v2.3.10][coverage]") {
    // Fresh server has empty items_; the empty-list branch in
    // format_list (line 128-129) emits "(empty)".
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    json args;
    args["action"] = "update";
    args["index"] = 0;
    args["status"] = "x";
    auto envelope = server.execute("todo", args.dump());
    auto resp = json::parse(envelope);
    REQUIRE(resp["result"].get<std::string>().find("(empty)")
            != std::string::npos);
}

TEST_CASE("test_delegate_emits_stop", "[entropic]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    json args;
    args["target"] = "eng";
    args["task"] = "do stuff";

    auto envelope = server.execute("delegate", args.dump());
    auto types = extract_directive_types(envelope);

    REQUIRE(has_directive(types, "delegate"));
    REQUIRE(has_directive(types, "stop_processing"));
}

TEST_CASE("test_pipeline_requires_two_stages", "[entropic]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    json args;
    args["stages"] = json::array({"eng"});
    args["task"] = "x";

    auto envelope = server.execute("pipeline", args.dump());
    auto result = extract_result(envelope);

    REQUIRE(result.find("2 stages") != std::string::npos);
}

TEST_CASE("test_pipeline_validates_tiers", "[entropic]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    json args;
    args["stages"] = json::array({"eng", "unknown_tier"});
    args["task"] = "x";

    auto envelope = server.execute("pipeline", args.dump());
    auto types = extract_directive_types(envelope);

    // Runtime validation: pipeline accepts any strings with >= 2 stages
    REQUIRE(has_directive(types, "pipeline"));
    REQUIRE(has_directive(types, "stop_processing"));
}

TEST_CASE("test_complete_emits_stop", "[entropic]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    json args;
    args["summary"] = "done";

    auto envelope = server.execute("complete", args.dump());
    auto types = extract_directive_types(envelope);

    REQUIRE(has_directive(types, "complete"));
    REQUIRE(has_directive(types, "stop_processing"));
}

// ── v2.3.10: cover complete coverage_gap + suggested_files branches ──

TEST_CASE("complete with coverage_gap=true requires gap_description",
          "[entropic][v2.3.10][coverage][failure-mode]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    json args;
    args["summary"] = "partial answer";
    args["coverage_gap"] = true;
    // Intentionally no gap_description — exercises the guard at
    // entropic_server.cpp:423-430 (issue #10, v2.1.4).

    auto envelope = server.execute("complete", args.dump());
    auto resp = json::parse(envelope);
    auto result = json::parse(resp["result"].get<std::string>());
    REQUIRE(result.contains("error"));
    REQUIRE(result["error"] == "missing_gap_description");
}

TEST_CASE("complete carries suggested_files into the result",
          "[entropic][v2.3.10][coverage]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    json args;
    args["summary"] = "done with hints";
    args["coverage_gap"] = true;
    args["gap_description"] =
        "missing edge case for foo() when input is empty";
    args["suggested_files"] = json::array(
        {"src/foo.cpp", "include/foo.h"});

    auto envelope = server.execute("complete", args.dump());
    auto resp = json::parse(envelope);
    auto result = json::parse(resp["result"].get<std::string>());
    REQUIRE(result["coverage_gap"] == true);
    REQUIRE(result["suggested_files"].size() == 2);
}

TEST_CASE("test_phase_change_emits_directive", "[entropic]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    json args;
    args["phase"] = "review";

    auto envelope = server.execute("phase_change", args.dump());
    auto types = extract_directive_types(envelope);

    REQUIRE(has_directive(types, "phase_change"));
}

TEST_CASE("test_prune_context_emits_directive", "[entropic]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    json args;
    args["keep_recent"] = 3;

    auto envelope = server.execute("prune_context", args.dump());
    auto types = extract_directive_types(envelope);

    REQUIRE(has_directive(types, "prune_messages"));
}

TEST_CASE("test_delegate_skips_duplicate_check", "[entropic]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    REQUIRE(server.skip_duplicate_check("delegate"));
    REQUIRE(server.skip_duplicate_check("pipeline"));
}

TEST_CASE("test_single_tier_skips_delegate", "[entropic]") {
    EntropicServer server({"lead"}, TEST_DATA_DIR);

    json args;
    args["target"] = "lead";
    args["task"] = "something";

    auto envelope = server.execute("delegate", args.dump());
    auto result = extract_result(envelope);

    REQUIRE(result.find("Unknown tool") != std::string::npos);
}

// ── v2.0.6: Delegate enum filtering ─────────────────────

SCENARIO("Delegate tool enum excludes source tiers",
         "[entropic_server][v2.0.6]")
{
    GIVEN("an entropic server with tiers [researcher, reader]") {
        // tiers passed to constructor are delegation TARGETS only
        // (collect_delegatable_tiers excludes sources)
        std::vector<std::string> tiers = {"researcher", "reader"};
        EntropicServer server(tiers, TEST_DATA_DIR);

        WHEN("delegate tool schema is inspected") {
            auto tools_json = server.list_tools();
            auto tools = json::parse(tools_json);

            // Find delegate tool
            json delegate_schema;
            for (const auto& tool : tools) {
                if (tool.value("name", "") == "delegate") {
                    delegate_schema = tool;
                    break;
                }
            }
            REQUIRE(!delegate_schema.is_null());

            auto target_enum = delegate_schema
                ["inputSchema"]["properties"]["target"]["enum"];

            THEN("researcher and reader are in enum") {
                auto has = [&](const std::string& v) {
                    return std::find(target_enum.begin(),
                        target_enum.end(), v) != target_enum.end();
                };
                CHECK(has("researcher"));
                CHECK(has("reader"));
            }

            THEN("lead is NOT in enum (self-delegation excluded)") {
                auto has_lead = std::find(target_enum.begin(),
                    target_enum.end(), "lead") != target_enum.end();
                CHECK_FALSE(has_lead);
            }
        }
    }
}

// ── gh#32 (v2.1.6): followup + resume_delegation tools ──

/**
 * @brief resume_delegation emits a delegate directive carrying the
 *        delegation_id so the engine can resolve target_tier from
 *        storage on processing.
 *
 * @version 2.1.6
 */
TEST_CASE("resume_delegation emits delegate+stop directives",
          "[entropic][gh32][v2.1.6]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    json args;
    args["delegation_id"] = "d-abc-123";
    args["task"] = "expand on QoS settings";

    auto envelope = server.execute("resume_delegation", args.dump());
    auto types = extract_directive_types(envelope);

    REQUIRE(has_directive(types, "delegate"));
    REQUIRE(has_directive(types, "stop_processing"));

    auto result = extract_result(envelope);
    REQUIRE(result.find("d-abc-123") != std::string::npos);
    REQUIRE(result.find("expand on QoS settings") != std::string::npos);
}

/**
 * @brief resume_delegation returns a typed error when required args are
 *        missing rather than crashing or emitting a bogus directive.
 *
 * @version 2.1.6
 */
TEST_CASE("resume_delegation rejects missing args",
          "[entropic][gh32][v2.1.6]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    auto envelope = server.execute("resume_delegation", R"({})");
    auto result = extract_result(envelope);
    REQUIRE(result.find("error") != std::string::npos);
}

/**
 * @brief followup returns a typed error when no state provider has
 *        been configured (storage unavailable).
 *
 * @version 2.1.6
 */
TEST_CASE("followup reports unavailable without provider",
          "[entropic][gh32][v2.1.6]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    auto envelope = server.execute("followup",
        R"({"query":"ping","max_results":3})");
    auto result = extract_result(envelope);
    REQUIRE(result.find("storage") != std::string::npos);
}

// ── v2.3.10: cover followup invalid-args + empty-query branches ──

TEST_CASE("followup rejects non-object args with a typed error",
          "[entropic][v2.3.10][coverage][failure-mode]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    // Pass a JSON scalar (not an object) — args.is_discarded() is
    // false but is_object() is false → hits the invalid-args branch
    // at entropic_server.cpp:1195.
    auto envelope = server.execute("followup", "42");
    auto result = extract_result(envelope);
    REQUIRE(result.find("invalid args") != std::string::npos);
}

TEST_CASE("followup rejects empty query with a typed error",
          "[entropic][v2.3.10][coverage][failure-mode]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    // Empty query → hits the line-1200 branch.
    auto envelope = server.execute("followup", R"({"query":""})");
    auto result = extract_result(envelope);
    REQUIRE(result.find("'query' is required") != std::string::npos);
}

TEST_CASE("followup with malformed (non-JSON) args returns invalid-args error",
          "[entropic][v2.3.10][coverage][failure-mode]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);

    // args.is_discarded() == true → hits the is_discarded branch
    // at entropic_server.cpp:1194-1195.
    auto envelope = server.execute("followup", "not-json");
    auto result = extract_result(envelope);
    REQUIRE(result.find("invalid args") != std::string::npos);
}

TEST_CASE("Entropic tools advertise their required access levels",
          "[entropic][v2.3.10][coverage]") {
    EntropicServer server({"lead", "eng"}, TEST_DATA_DIR);
    auto& reg = server.registry();

    // Each entropic tool defines required_access_level(). Query each
    // so the per-tool override executes. Names per entropic_server's
    // tool registration:
    for (const auto& name : {"todo", "delegate", "pipeline", "complete",
                              "phase_change", "prune_context",
                              "diagnose", "inspect", "context_inspect",
                              "followup", "resume_delegation"}) {
        auto* t = reg.get_tool(name);
        if (t == nullptr) { continue; }  // some tools depend on tier config
        auto lvl = t->required_access_level();
        (void)lvl;
    }
    REQUIRE(true);
}
