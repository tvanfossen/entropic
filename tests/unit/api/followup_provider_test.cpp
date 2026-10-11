// SPDX-License-Identifier: Apache-2.0
/**
 * @file followup_provider_test.cpp
 * @brief gh#203 (v2.14.0): entropic.followup returns the shape it declares,
 *        end to end — storage -> the facade's search_delegations provider ->
 *        the tool.
 *
 * Two layers disagreed with their own contracts: the provider returned
 * storage's raw rows (`id`, `result_summary`, ...) where i_mcp_server.h
 * declares `{delegation_id, target_tier, summary, completed_at}`, and the
 * tool passed that bare array through where followup.json promised
 * `{"results": [...]}`. A model told to "relay delegation_id" got no such
 * key. This drives the real path, not a mock provider.
 *
 * @version 2.14.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>
#include "engine_handle.h"  // white-box: storage, server_manager

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

SCENARIO("gh#203: followup returns delegation_id and summary from real storage",
         "[api][followup][gh203][2.14.0]") {
    const auto dir = fs::temp_directory_path()
        / ("entropic_followup_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    entropic_handle_t h = nullptr;
    REQUIRE(entropic_create(&h) == ENTROPIC_OK);
    const std::string cfg = R"({"log_level":"WARN","mcp":{"working_dir":")"
        + dir.string() + R"("}})";
    REQUIRE(entropic_configure(h, cfg.c_str()) == ENTROPIC_OK);
    REQUIRE(entropic_storage_open(h, (dir / "s.db").c_str()) == ENTROPIC_OK);

    GIVEN("one completed delegation whose summary mentions a token") {
        auto parent = h->storage->create_conversation("p");
        std::string del_id, child;
        REQUIRE(h->storage->create_delegation(parent, "lead", "eng", "find it",
                                              3, del_id, child));
        h->storage->complete_delegation(del_id, "completed", "answer: magic-token at a.cpp:12");

        WHEN("followup searches for the token") {
            auto env = json::parse(h->server_manager->execute(
                "entropic.followup", R"({"query":"magic-token"})"));
            auto out = json::parse(env.at("result").get<std::string>());
            INFO(out.dump());
            THEN("it is the declared envelope, carrying the interface's fields") {
                REQUIRE(out.contains("results"));
                REQUIRE(out["results"].size() == 1);
                const auto& r = out["results"][0];
                CHECK(r["delegation_id"] == del_id);
                CHECK(r["target_tier"] == "eng");
                CHECK(r["summary"] == "answer: magic-token at a.cpp:12");
                CHECK(r.size() == 4);
            }
        }
    }
    entropic_destroy(h);
    fs::remove_all(dir);
}
