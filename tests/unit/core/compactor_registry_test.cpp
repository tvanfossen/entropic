// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_compactor_registry.cpp
 * @brief CompactorRegistry unit tests.
 * @version 1.9.9
 */

#include <entropic/core/compactor_registry.h>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <thread>
#include <vector>

using namespace entropic;

// ── Helpers ──────────────────────────────────────────────

/**
 * @brief Create a message with role and content.
 * @param role Message role.
 * @param content Message content.
 * @return Message struct.
 * @internal
 * @version 1.9.9
 */
static Message make_msg(const std::string& role,
                        const std::string& content) {
    Message m;
    m.role = role;
    m.content = content;
    return m;
}

/**
 * @brief Build a test message set over compaction threshold.
 * @return Vector of messages.
 * @internal
 * @version 1.9.9
 */
static std::vector<Message> make_test_messages() {
    std::vector<Message> msgs;
    msgs.push_back(make_msg("system", "system prompt"));
    auto user_msg = make_msg("user", "original task");
    user_msg.metadata["source"] = "user";
    msgs.push_back(user_msg);
    msgs.push_back(make_msg("assistant", "response text"));
    return msgs;
}

/// @brief Counter incremented by custom compactors for verification.
static int g_custom_call_count = 0;

/**
 * @brief C compactor that returns a single summary message.
 * @internal
 * @version 1.9.9
 */
static int simple_compactor(
    const char* /*messages_json*/,
    const char* /*config_json*/,
    char** out_messages,
    char** out_summary,
    void* /*user_data*/) {
    ++g_custom_call_count;
    const char* result =
        R"([{"role":"system","content":"compacted"}])";
    *out_messages = strdup(result);
    *out_summary = strdup("custom summary");
    return 0;
}

/**
 * @brief C compactor that always fails (returns non-zero).
 * @internal
 * @version 1.9.9
 */
static int failing_compactor(
    const char* /*messages_json*/,
    const char* /*config_json*/,
    char** /*out_messages*/,
    char** /*out_summary*/,
    void* /*user_data*/) {
    return -1;
}

static std::string g_captured_config;

/**
 * @brief C compactor that captures config_json for inspection.
 * @internal
 * @version 1.9.9
 */
static int capturing_compactor(
    const char* /*messages_json*/,
    const char* config_json,
    char** out_messages,
    char** out_summary,
    void* /*user_data*/) {
    g_captured_config = config_json;
    *out_messages =
        strdup(R"([{"role":"system","content":"captured"}])");
    *out_summary = strdup("captured");
    return 0;
}

// ── Tests ────────────────────────────────────────────────

TEST_CASE("Default compactor runs when no custom registered",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    auto msgs = make_test_messages();
    auto result = reg.compact("eng", msgs, cfg);

    REQUIRE(result.compacted);
    REQUIRE(result.custom_compactor_used == false);
    REQUIRE(result.compactor_source == "default");
    REQUIRE(result.identity == "eng");
}

TEST_CASE("Per-identity compactor overrides default",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    g_custom_call_count = 0;
    reg.register_compactor("eng", simple_compactor, nullptr);

    auto msgs = make_test_messages();
    auto result = reg.compact("eng", msgs, cfg);

    REQUIRE(result.compacted);
    REQUIRE(result.custom_compactor_used);
    REQUIRE(result.compactor_source == "eng");
    REQUIRE(g_custom_call_count == 1);
}

TEST_CASE("Global custom compactor applies to unregistered identities",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    g_custom_call_count = 0;
    reg.register_compactor("", simple_compactor, nullptr);

    auto msgs = make_test_messages();
    auto result = reg.compact("qa", msgs, cfg);

    REQUIRE(result.compacted);
    REQUIRE(result.compactor_source == "global_custom");
    REQUIRE(g_custom_call_count == 1);
}

TEST_CASE("Per-identity takes precedence over global",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    g_custom_call_count = 0;
    reg.register_compactor("", simple_compactor, nullptr);
    reg.register_compactor("eng", simple_compactor, nullptr);

    auto msgs = make_test_messages();
    auto result = reg.compact("eng", msgs, cfg);

    REQUIRE(result.compactor_source == "eng");
}

TEST_CASE("Deregistration falls back to global",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    reg.register_compactor("", simple_compactor, nullptr);
    reg.register_compactor("eng", simple_compactor, nullptr);
    reg.deregister_compactor("eng");

    auto msgs = make_test_messages();
    auto result = reg.compact("eng", msgs, cfg);

    REQUIRE(result.compactor_source == "global_custom");
}

TEST_CASE("Deregistration falls back to default",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    reg.register_compactor("eng", simple_compactor, nullptr);
    reg.deregister_compactor("eng");

    auto msgs = make_test_messages();
    auto result = reg.compact("eng", msgs, cfg);

    REQUIRE(result.compactor_source == "default");
    REQUIRE(result.custom_compactor_used == false);
}

TEST_CASE("Re-registration replaces previous",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    g_custom_call_count = 0;
    reg.register_compactor("eng", failing_compactor, nullptr);
    reg.register_compactor("eng", simple_compactor, nullptr);

    auto msgs = make_test_messages();
    auto result = reg.compact("eng", msgs, cfg);

    // simple_compactor should run, not failing_compactor
    REQUIRE(result.compacted);
    REQUIRE(result.custom_compactor_used);
    REQUIRE(g_custom_call_count == 1);
}

TEST_CASE("Custom compactor failure falls back to default",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    reg.register_compactor("eng", failing_compactor, nullptr);

    auto msgs = make_test_messages();
    auto result = reg.compact("eng", msgs, cfg);

    REQUIRE(result.compacted);
    REQUIRE(result.compactor_source == "default");
    REQUIRE(result.custom_compactor_used == false);
}

TEST_CASE("Custom compactor receives identity in config JSON",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    g_captured_config.clear();
    reg.register_compactor("eng", capturing_compactor, nullptr);

    auto msgs = make_test_messages();
    reg.compact("eng", msgs, cfg);

    REQUIRE(g_captured_config.find("\"identity\":\"eng\"")
            != std::string::npos);
}

TEST_CASE("has_custom_compactor returns correct state",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    REQUIRE_FALSE(reg.has_custom_compactor("eng"));

    reg.register_compactor("eng", simple_compactor, nullptr);
    REQUIRE(reg.has_custom_compactor("eng"));

    reg.deregister_compactor("eng");
    REQUIRE_FALSE(reg.has_custom_compactor("eng"));
}

TEST_CASE("has_custom_compactor sees global",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    reg.register_compactor("", simple_compactor, nullptr);
    REQUIRE(reg.has_custom_compactor("any_identity"));
}

TEST_CASE("NULL compactor rejected",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    auto rc = reg.register_compactor("eng", nullptr, nullptr);
    REQUIRE(rc == ENTROPIC_ERROR_INVALID_CONFIG);
}

TEST_CASE("Concurrent registration and dispatch",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    CompactorRegistry reg(cm);

    auto msgs = make_test_messages();
    bool dispatch_ok = true;
    bool register_ok = true;

    std::thread dispatcher([&]() {
        for (int i = 0; i < 100; ++i) {
            auto r = reg.compact("eng", msgs, cfg);
            if (!r.compacted) dispatch_ok = false;
        }
    });

    std::thread registrar([&]() {
        for (int i = 0; i < 100; ++i) {
            reg.register_compactor("eng",
                                   simple_compactor, nullptr);
            reg.deregister_compactor("eng");
        }
    });

    dispatcher.join();
    registrar.join();

    REQUIRE(dispatch_ok);
    REQUIRE(register_ok);
}

TEST_CASE("compact_messages returns populated result",
          "[compactor_registry]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);

    auto msgs = make_test_messages();
    auto result = cm.compact_messages(msgs);

    REQUIRE(result.compacted);
    REQUIRE(result.compactor_source == "default");
    REQUIRE_FALSE(result.messages.empty());
    REQUIRE(result.old_token_count > 0);
}
