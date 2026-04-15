// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_compaction.cpp
 * @brief TokenCounter and CompactionManager unit tests.
 * @version 1.8.4
 */

#include <entropic/core/compaction.h>
#include <catch2/catch_test_macros.hpp>

using namespace entropic;

// ── Helper ───────────────────────────────────────────────

/**
 * @brief Create a message with role and content.
 * @param role Message role.
 * @param content Message content.
 * @return Message struct.
 * @internal
 * @version 1.8.4
 */
static Message make_msg(const std::string& role,
                        const std::string& content) {
    Message m;
    m.role = role;
    m.content = content;
    return m;
}

// ── TokenCounter tests ───────────────────────────────────

TEST_CASE("TokenCounter heuristic ~4 chars/token",
          "[compaction]") {
    TokenCounter tc(1000);
    Message m = make_msg("user", "Hello world!"); // 12 chars
    int count = tc.count_message(m);
    // 12/4 + 1 = 4 content + 4 role = 8
    REQUIRE(count == 8);
}

TEST_CASE("TokenCounter cache returns same value",
          "[compaction]") {
    TokenCounter tc(1000);
    Message m = make_msg("user", "test");
    int a = tc.count_message(m);
    int b = tc.count_message(m);
    REQUIRE(a == b);
}

TEST_CASE("TokenCounter count_messages sums correctly",
          "[compaction]") {
    TokenCounter tc(1000);
    std::vector<Message> msgs = {
        make_msg("system", "sys"),
        make_msg("user", "hello"),
    };
    int total = tc.count_messages(msgs);
    REQUIRE(total > 0);
    REQUIRE(total == tc.count_message(msgs[0])
                   + tc.count_message(msgs[1]));
}

TEST_CASE("TokenCounter usage_percent", "[compaction]") {
    TokenCounter tc(100);
    // 400 chars = ~100 tokens content + overhead
    Message big = make_msg("user", std::string(400, 'x'));
    std::vector<Message> msgs = {big};
    float usage = tc.usage_percent(msgs);
    REQUIRE(usage > 0.5f);
}

TEST_CASE("TokenCounter clear_cache resets", "[compaction]") {
    TokenCounter tc(1000);
    Message m = make_msg("user", "test");
    tc.count_message(m);
    tc.clear_cache();
    // Should still work after clear
    int count = tc.count_message(m);
    REQUIRE(count > 0);
}

// ── CompactionManager tests ──────────────────────────────

TEST_CASE("Compaction below threshold returns unchanged",
          "[compaction]") {
    TokenCounter tc(10000);
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    std::vector<Message> msgs = {
        make_msg("system", "sys"),
        make_msg("user", "hello"),
    };
    auto result = cm.check_and_compact(msgs);
    REQUIRE_FALSE(result.compacted);
    REQUIRE(msgs.size() == 2);
}

TEST_CASE("Compaction above threshold compacts",
          "[compaction]") {
    TokenCounter tc(200); // Moderate window
    CompactionConfig cfg;
    cfg.threshold_percent = 0.3f;
    CompactionManager cm(cfg, tc);

    // Build enough content that stripping tool results + old assistants
    // saves more than the summary overhead
    std::string filler(300, 'x');
    std::vector<Message> msgs;
    msgs.push_back(make_msg("system", "system prompt"));
    auto user_msg = make_msg("user", "original task");
    user_msg.metadata["source"] = "user";
    msgs.push_back(user_msg);
    msgs.push_back(make_msg("assistant", filler));
    msgs.push_back(make_msg("user", "[tool result] " + filler));
    msgs.push_back(make_msg("assistant", filler));

    auto result = cm.check_and_compact(msgs);
    REQUIRE(result.compacted);
    REQUIRE(result.messages_summarized > 0);
}

TEST_CASE("Force compaction bypasses threshold",
          "[compaction]") {
    TokenCounter tc(100000); // Huge window
    CompactionConfig cfg;
    CompactionManager cm(cfg, tc);
    std::vector<Message> msgs = {
        make_msg("system", "sys"),
        make_msg("user", "hello"),
        make_msg("assistant", "world"),
    };
    auto result = cm.check_and_compact(msgs, true);
    REQUIRE(result.compacted);
}

TEST_CASE("System message preserved in compaction",
          "[compaction]") {
    TokenCounter tc(50);
    CompactionConfig cfg;
    cfg.threshold_percent = 0.1f;
    CompactionManager cm(cfg, tc);

    std::vector<Message> msgs = {
        make_msg("system", "I am the system"),
        make_msg("assistant", "blah blah blah blah blah"),
        make_msg("assistant", "more stuff here"),
    };
    cm.check_and_compact(msgs);
    REQUIRE(msgs[0].role == "system");
    REQUIRE(msgs[0].content == "I am the system");
}

TEST_CASE("User messages preserved in compaction",
          "[compaction]") {
    TokenCounter tc(50);
    CompactionConfig cfg;
    cfg.threshold_percent = 0.1f;
    CompactionManager cm(cfg, tc);

    auto user_msg = make_msg("user", "my task");
    user_msg.metadata["source"] = "user";

    std::vector<Message> msgs = {
        make_msg("system", "sys"),
        user_msg,
        make_msg("assistant", "long response here"),
        make_msg("user", "[tool output]"),
    };
    cm.check_and_compact(msgs);

    bool found = false;
    for (const auto& m : msgs) {
        auto it = m.metadata.find("source");
        if (it != m.metadata.end() && it->second == "user") {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("Compaction disabled warns", "[compaction]") {
    TokenCounter tc(50);
    CompactionConfig cfg;
    cfg.enabled = false;
    cfg.threshold_percent = 0.1f;
    CompactionManager cm(cfg, tc);

    std::vector<Message> msgs = {
        make_msg("system", "sys"),
        make_msg("assistant", "lots of content here"),
    };
    size_t before = msgs.size();
    auto result = cm.check_and_compact(msgs);
    REQUIRE_FALSE(result.compacted);
    REQUIRE(msgs.size() == before);
}
