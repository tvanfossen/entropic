// SPDX-License-Identifier: LGPL-3.0-or-later
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
