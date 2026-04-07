/**
 * @file test_e10_custom_compactor.cpp
 * @brief E10: Custom compactor invoked instead of default when registered.
 *
 * Exercises CompactorRegistry directly: registers a custom compactor,
 * runs compaction on a padded message list, and verifies the custom
 * compactor was called and its output is reflected in the result.
 *
 * This is a subsystem-level test — it does not require the engine loop.
 * It validates the feature at the CompactorRegistry abstraction level.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"
#include <entropic/core/compactor_registry.h>

#include <cstring>

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Custom compactor state ─────────────────────────────────

/// @brief Flag set when custom compactor is invoked.
/// @internal
/// @version 1.10.2
static bool g_custom_compactor_called = false;

/**
 * @brief Custom C compactor that sets a flag and returns a summary.
 * @param messages_json Input messages JSON (unused).
 * @param config_json Config JSON (unused).
 * @param out_messages Output: minimal compacted messages JSON.
 * @param out_summary Output: custom summary text.
 * @param user_data Unused.
 * @return 0 on success.
 * @callback
 * @version 1.10.2
 */
static int custom_test_compactor(
    const char* /*messages_json*/,
    const char* /*config_json*/,
    char** out_messages,
    char** out_summary,
    void* /*user_data*/) {
    g_custom_compactor_called = true;

    const char* msgs = R"([{"role":"system","content":"[CUSTOM] Compacted by test compactor"}])";
    *out_messages = static_cast<char*>(malloc(strlen(msgs) + 1));
    std::memcpy(*out_messages, msgs, strlen(msgs) + 1);

    const char* summary = "[CUSTOM] Compacted by test compactor";
    *out_summary = static_cast<char*>(malloc(strlen(summary) + 1));
    std::memcpy(*out_summary, summary, strlen(summary) + 1);
    return 0;
}

// ── E10: Custom compactor test ─────────────────────────────

SCENARIO("Custom compactor is invoked instead of default when registered",
         "[model][custom_compactor]")
{
    GIVEN("a CompactorRegistry with a custom compactor registered") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e10_custom_compactor");
        g_custom_compactor_called = false;

        CompactionConfig cc;
        cc.threshold_percent = 0.3f;
        cc.preserve_recent_turns = 1;
        TokenCounter counter(16384);
        CompactionManager default_mgr(cc, counter);
        CompactorRegistry registry(default_mgr);

        auto rc = registry.register_compactor(
            "", custom_test_compactor, nullptr);
        REQUIRE(rc == ENTROPIC_OK);
        REQUIRE(registry.has_custom_compactor(""));

        WHEN("compaction runs on padded messages") {
            std::vector<Message> messages;
            Message sys;
            sys.role = "system";
            sys.content = "You are a helpful assistant.";
            messages.push_back(std::move(sys));

            for (int i = 0; i < 30; ++i) {
                Message u;
                u.role = "user";
                u.content = "Pad " + std::to_string(i)
                    + " " + std::string(300, 'x');
                Message a;
                a.role = "assistant";
                a.content = "Ack " + std::to_string(i)
                    + " " + std::string(300, 'y');
                messages.push_back(std::move(u));
                messages.push_back(std::move(a));
            }

            auto result = registry.compact("", messages, cc);

            THEN("custom compactor was invoked with correct metadata") {
                CHECK(g_custom_compactor_called);
                CHECK(result.custom_compactor_used);
                CHECK(result.summary.find("[CUSTOM]")
                      != std::string::npos);
                end_test_log();
            }
        }
    }
}
