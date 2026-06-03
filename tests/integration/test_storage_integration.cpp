// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_storage_integration.cpp
 * @brief Integration tests for storage wiring into core engine.
 *
 * Tests that StorageInterface callbacks are invoked correctly when
 * delegations and compactions occur through the engine.
 *
 * @version 1.8.8
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/core/compaction.h>
#include <entropic/core/delegation.h>
#include <entropic/core/engine_types.h>
#include <entropic/storage/backend.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── Test storage callbacks ────────────────────────────────

/**
 * @brief Test harness that records storage calls.
 * @internal
 * @version 1.8.8
 */
struct StorageHarness {
    entropic::SqliteStorageBackend storage;
    int snapshot_calls = 0;
    int create_delegation_calls = 0;
    int complete_delegation_calls = 0;
    std::string last_delegation_id;
    std::string last_child_id;

    /**
     * @brief Construct with temp database.
     * @param path Database path.
     * @version 1.8.8
     */
    explicit StorageHarness(const fs::path& path) : storage(path) {
        storage.initialize();
    }
};

/**
 * @brief Snapshot callback that delegates to real storage.
 * @internal
 * @version 1.8.8
 */
static bool test_save_snapshot(
        const char* conv_id, const char* msgs_json, void* ud) {
    auto* h = static_cast<StorageHarness*>(ud);
    h->snapshot_calls++;
    return h->storage.save_snapshot(conv_id, msgs_json);
}

/**
 * @brief Create delegation callback that delegates to real storage.
 * @internal
 * @version 1.8.8
 */
static bool test_create_delegation(
        const char* parent_id, const char* del_tier,
        const char* target_tier, const char* task,
        int max_turns, std::string& del_id,
        std::string& child_id, void* ud) {
    auto* h = static_cast<StorageHarness*>(ud);
    h->create_delegation_calls++;
    bool ok = h->storage.create_delegation(
        parent_id, del_tier, target_tier, task, max_turns,
        del_id, child_id);
    h->last_delegation_id = del_id;
    h->last_child_id = child_id;
    return ok;
}

/**
 * @brief Complete delegation callback.
 * @internal
 * @version 1.8.8
 */
static bool test_complete_delegation(
        const char* del_id, const char* status,
        const char* summary, void* ud) {
    auto* h = static_cast<StorageHarness*>(ud);
    h->complete_delegation_calls++;
    return h->storage.complete_delegation(
        del_id, status,
        summary ? std::optional<std::string>(summary) : std::nullopt);
}

// ── Tests ─────────────────────────────────────────────────

SCENARIO("CompactionManager saves snapshot via StorageInterface",
         "[integration][storage]") {
    GIVEN("A CompactionManager with storage wired") {
        auto db_path = fs::temp_directory_path() / "entropic_test" /
                       "integration_compact.db";
        StorageHarness harness(db_path);

        // Create a conversation to snapshot
        auto conv_id = harness.storage.create_conversation("Test conv");

        entropic::StorageInterface si{};
        si.save_snapshot = test_save_snapshot;
        si.user_data = &harness;

        entropic::CompactionConfig cfg;
        cfg.save_full_history = true;
        cfg.threshold_percent = 0.1f; // Low threshold to trigger
        entropic::TokenCounter counter(100); // Small window
        entropic::CompactionManager mgr(cfg, counter);
        mgr.set_storage(&si);

        WHEN("compaction triggers with a conversation_id") {
            // Fill messages to exceed threshold
            std::vector<entropic::Message> msgs;
            for (int i = 0; i < 20; ++i) {
                entropic::Message m;
                m.role = "user";
                m.content = "Message " + std::to_string(i) +
                            " with enough content to use tokens";
                msgs.push_back(std::move(m));
            }

            mgr.check_and_compact(msgs, false, conv_id);

            THEN("save_snapshot was called") {
                REQUIRE(harness.snapshot_calls == 1);
            }
        }

        fs::remove(db_path);
    }
}

SCENARIO("StorageInterface delegation callbacks are invoked",
         "[integration][storage]") {
    GIVEN("A StorageInterface with delegation callbacks") {
        auto db_path = fs::temp_directory_path() / "entropic_test" /
                       "integration_deleg.db";
        StorageHarness harness(db_path);

        auto parent_id = harness.storage.create_conversation("Parent");

        entropic::StorageInterface si{};
        si.create_delegation = test_create_delegation;
        si.complete_delegation = test_complete_delegation;
        si.user_data = &harness;

        WHEN("create_delegation is called directly") {
            std::string del_id, child_id;
            bool ok = si.create_delegation(
                parent_id.c_str(), "lead", "eng",
                "Build feature", 10, del_id, child_id,
                si.user_data);

            THEN("delegation record is created in storage") {
                REQUIRE(ok);
                REQUIRE(harness.create_delegation_calls == 1);
                REQUIRE(del_id.size() == 36);
                REQUIRE(child_id.size() == 36);
            }

            AND_WHEN("complete_delegation is called") {
                si.complete_delegation(
                    del_id.c_str(), "completed",
                    "All done", si.user_data);

                THEN("completion is recorded") {
                    REQUIRE(harness.complete_delegation_calls == 1);

                    // Verify in actual database
                    std::string result;
                    harness.storage.get_delegations(parent_id, result);
                    auto arr = json::parse(result);
                    REQUIRE(arr.size() == 1);
                    REQUIRE(arr[0]["status"] == "completed");
                }
            }
        }

        fs::remove(db_path);
    }
}

// ── gh#48 (regression, v2.7.4): combined real-storage delegation path ─────
//
// The unit test (MockStorage) proves build_child_context propagates the
// parent_conversation_id to create_delegation; the scenario above proves the
// storage layer persists a delegation given a *hardcoded* valid parent. But
// neither exercises the COMBINED path — the real DelegationManager feeding the
// real SqliteStorageBackend FK guard — which is exactly where gh#48 regressed
// (build_child_context dropped the parent id, the guard rejected the empty
// INSERT, the table stayed empty, and entropic.followup always came back
// empty). Testing the layers in isolation is the gh#88 trap; this is the
// end-to-end guard that would have caught the regression.

/**
 * @brief Always-valid tier resolution for the gh#48 combined-path test.
 * @internal
 * @version 2.7.4
 */
static entropic::ChildContextInfo gh48_resolve_tier(
        const std::string& /*tier*/, void* /*ud*/) {
    entropic::ChildContextInfo info;
    info.valid = true;
    info.system_prompt = "You are an engineer.";
    info.explicit_completion = true;
    info.completion_instructions = "Use entropic.complete when done.";
    return info;
}

/**
 * @brief tier_exists stub — every tier resolves for this test.
 * @internal
 * @version 2.7.4
 */
static bool gh48_tier_exists(const std::string& /*tier*/, void* /*ud*/) {
    return true;
}

/**
 * @brief No-op child loop — completes the child without invoking a model.
 * @internal
 * @version 2.7.4
 */
static void gh48_run_child(entropic::LoopContext& ctx, void* /*ud*/) {
    entropic::Message a;
    a.role = "assistant";
    a.content = "done";
    ctx.messages.push_back(std::move(a));
    ctx.state = entropic::AgentState::COMPLETE;
    ctx.metrics.iterations = 1;
}

SCENARIO("gh#48: a manager-driven delegation persists a row the FK accepts "
         "and followup retrieves",
         "[integration][storage][delegation][gh48]") {
    GIVEN("a real SQLite store + root conversation + manager wired to it") {
        auto db_path = fs::temp_directory_path() / "entropic_test" /
                       "gh48_combined_deleg.db";
        fs::remove(db_path);
        StorageHarness harness(db_path);

        // The root conversation row — the parent the child delegation
        // references (init_session_conversation's job in production).
        auto root_conv = harness.storage.create_conversation("root");
        REQUIRE_FALSE(root_conv.empty());

        // Real StorageInterface backed by the real SQLite backend.
        entropic::StorageInterface si{};
        si.create_delegation = test_create_delegation;
        si.complete_delegation = test_complete_delegation;
        si.user_data = &harness;

        // Real DelegationManager — the layer gh#48 regressed in.
        entropic::TierResolutionInterface tri{};
        tri.resolve_tier = gh48_resolve_tier;
        tri.tier_exists = gh48_tier_exists;
        entropic::DelegationManager mgr(gh48_run_child, nullptr, tri);
        mgr.set_storage(&si);

        WHEN("execute_delegation runs with the root conversation as parent") {
            entropic::LoopContext parent;
            parent.conversation_id = root_conv;
            auto result = mgr.execute_delegation(
                parent, "eng", "Build feature");

            THEN("the delegations row persists under root and followup finds it") {
                // RED on the regression: build_child_context drops the parent
                // id -> create_delegation refuses the empty INSERT -> no row ->
                // get_delegations returns []. The callback still fires (returns
                // false internally), so the count check passes either way; the
                // array-size check is the discriminator.
                INFO("result.success=" << result.success
                     << " create_calls=" << harness.create_delegation_calls);
                CHECK(result.success);
                REQUIRE(harness.create_delegation_calls == 1);

                std::string json_out;
                harness.storage.get_delegations(root_conv, json_out);
                auto arr = json::parse(json_out);
                REQUIRE(arr.size() == 1);
                CHECK(arr[0]["parent_conversation_id"] == root_conv);
                CHECK(arr[0]["target_tier"] == "eng");
                CHECK(arr[0]["task"] == "Build feature");
            }
        }

        fs::remove(db_path);
    }
}

SCENARIO("Null StorageInterface is safe",
         "[integration][storage]") {
    GIVEN("A CompactionManager with no storage") {
        entropic::CompactionConfig cfg;
        cfg.save_full_history = true;
        cfg.threshold_percent = 0.1f;
        entropic::TokenCounter counter(100);
        entropic::CompactionManager mgr(cfg, counter);
        // No set_storage call — storage_ remains nullptr

        WHEN("compaction triggers") {
            std::vector<entropic::Message> msgs;
            for (int i = 0; i < 20; ++i) {
                entropic::Message m;
                m.role = "user";
                m.content = "Message content that uses tokens";
                msgs.push_back(std::move(m));
            }

            auto result = mgr.check_and_compact(msgs, false, "conv-123");

            THEN("compaction succeeds without crash") {
                REQUIRE(result.compacted);
            }
        }
    }
}
