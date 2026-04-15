// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_session_persistence.cpp
 * @brief BDD subsystem test — save and load conversation with real model.
 *
 * Validates that a model-generated response can be persisted to a
 * SQLite database via SqliteStorageBackend and loaded back with
 * content intact.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"

#include <entropic/storage/backend.h>

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test: Session Persistence ───────────────────────────────

SCENARIO("Conversation with model output survives save and load",
         "[model][session_persistence]")
{
    GIVEN("a model loaded and temp SQLite database created") {
        REQUIRE(g_ctx.initialized);
        start_test_log("test_session_persistence");

        auto db_path = fs::temp_directory_path()
            / "entropic_test_session.db";
        SqliteStorageBackend backend(db_path);
        REQUIRE(backend.initialize());

        WHEN("a model response is generated and saved") {
            auto result = g_ctx.orchestrator->generate(
                make_messages("You are a helpful assistant.",
                              "What is 2+2?"),
                test_gen_params(), g_ctx.default_tier);

            auto conv_id = backend.create_conversation(
                "model_test");

            json messages = json::array();
            messages.push_back({{"role", "user"},
                                {"content", "What is 2+2?"}});
            messages.push_back({{"role", "assistant"},
                                {"content", result.content}});
            REQUIRE(backend.save_messages(
                conv_id, messages.dump()));

            std::string loaded_json;
            REQUIRE(backend.load_conversation(
                conv_id, loaded_json));

            THEN("the loaded conversation contains the answer") {
                auto loaded = json::parse(loaded_json);
                CHECK(loaded_json.find("4")
                      != std::string::npos);
                backend.close();
                fs::remove(db_path);
                end_test_log();
            }
        }
    }
}
