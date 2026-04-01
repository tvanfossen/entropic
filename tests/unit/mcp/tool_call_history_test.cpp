/**
 * @file test_tool_call_history.cpp
 * @brief ToolCallHistory ring buffer unit tests.
 * @version 1.9.12
 */

#include <entropic/mcp/tool_call_history.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <thread>
#include <vector>

using namespace entropic;
using json = nlohmann::json;

// ── Helper ──────────────────────────────────────────────────────

/**
 * @brief Build a ToolCallRecord with the given sequence number.
 * @param seq Sequence number.
 * @return Record with test data.
 * @internal
 * @version 1.9.12
 */
static ToolCallRecord make_record(size_t seq) {
    return {seq, "tool." + std::to_string(seq),
            "key1, key2", "success", "result " + std::to_string(seq),
            1.5, "", static_cast<int>(seq)};
}

// ── Tests ───────────────────────────────────────────────────────

TEST_CASE("test_empty_history", "[tool_call_history]") {
    ToolCallHistory h(10);
    REQUIRE(h.size() == 0);
    REQUIRE(h.recent(5).empty());
    REQUIRE(h.all().empty());
}

TEST_CASE("test_record_single", "[tool_call_history]") {
    ToolCallHistory h(10);
    h.record(make_record(1));
    REQUIRE(h.size() == 1);
    auto r = h.recent(1);
    REQUIRE(r.size() == 1);
    REQUIRE(r[0].sequence == 1);
}

TEST_CASE("test_record_fills_to_capacity", "[tool_call_history]") {
    ToolCallHistory h(5);
    for (size_t i = 0; i < 5; ++i) {
        h.record(make_record(i));
    }
    REQUIRE(h.size() == 5);
}

TEST_CASE("test_record_overwrites_oldest", "[tool_call_history]") {
    ToolCallHistory h(3);
    for (size_t i = 0; i < 5; ++i) {
        h.record(make_record(i));
    }
    REQUIRE(h.size() == 3);

    auto entries = h.all();
    REQUIRE(entries.size() == 3);
    // Oldest two (0, 1) should be gone
    REQUIRE(entries[0].sequence == 2);
    REQUIRE(entries[1].sequence == 3);
    REQUIRE(entries[2].sequence == 4);
}

TEST_CASE("test_recent_newest_first", "[tool_call_history]") {
    ToolCallHistory h(10);
    for (size_t i = 0; i < 5; ++i) {
        h.record(make_record(i));
    }
    auto r = h.recent(3);
    REQUIRE(r.size() == 3);
    REQUIRE(r[0].sequence == 4);  // newest
    REQUIRE(r[1].sequence == 3);
    REQUIRE(r[2].sequence == 2);
}

TEST_CASE("test_recent_more_than_available", "[tool_call_history]") {
    ToolCallHistory h(10);
    for (size_t i = 0; i < 3; ++i) {
        h.record(make_record(i));
    }
    auto r = h.recent(100);
    REQUIRE(r.size() == 3);
}

TEST_CASE("test_all_oldest_first", "[tool_call_history]") {
    ToolCallHistory h(10);
    for (size_t i = 0; i < 5; ++i) {
        h.record(make_record(i));
    }
    auto entries = h.all();
    REQUIRE(entries.size() == 5);
    for (size_t i = 0; i < 5; ++i) {
        REQUIRE(entries[i].sequence == i);
    }
}

TEST_CASE("test_to_json_valid", "[tool_call_history]") {
    ToolCallHistory h(10);
    for (size_t i = 0; i < 3; ++i) {
        h.record(make_record(i));
    }
    auto j_str = h.to_json(0);
    auto arr = json::parse(j_str);
    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 3);
    REQUIRE(arr[0].contains("sequence"));
    REQUIRE(arr[0].contains("tool_name"));
}

TEST_CASE("test_to_json_limit", "[tool_call_history]") {
    ToolCallHistory h(10);
    for (size_t i = 0; i < 8; ++i) {
        h.record(make_record(i));
    }
    auto j_str = h.to_json(3);
    auto arr = json::parse(j_str);
    REQUIRE(arr.size() == 3);
}

TEST_CASE("test_params_summary_keys_only", "[tool_call_history]") {
    auto summary = summarize_params(
        R"({"path":"/src/main.cpp","content":"...10KB..."})");
    REQUIRE(summary == "content, path");
}

TEST_CASE("test_result_summary_truncated", "[tool_call_history]") {
    std::string long_text(300, 'x');
    auto truncated = truncate_result(long_text, 200);
    REQUIRE(truncated.size() == 203);  // 200 + "..."
    REQUIRE(truncated.substr(200) == "...");
}

TEST_CASE("test_result_summary_short_unchanged", "[tool_call_history]") {
    auto result = truncate_result("short", 200);
    REQUIRE(result == "short");
}

TEST_CASE("test_sequence_monotonic", "[tool_call_history]") {
    ToolCallHistory h(5);
    for (size_t i = 10; i < 15; ++i) {
        h.record(make_record(i));
    }
    auto entries = h.all();
    for (size_t i = 1; i < entries.size(); ++i) {
        REQUIRE(entries[i].sequence > entries[i - 1].sequence);
    }
}

TEST_CASE("test_concurrent_write_read", "[tool_call_history]") {
    ToolCallHistory h(100);
    constexpr int per_thread = 50;
    constexpr int n_writers = 4;

    std::vector<std::thread> writers;
    for (int t = 0; t < n_writers; ++t) {
        writers.emplace_back([&h, t]() {
            for (int i = 0; i < per_thread; ++i) {
                auto seq = static_cast<size_t>(
                    t * per_thread + i);
                h.record(make_record(seq));
            }
        });
    }

    std::vector<std::thread> readers;
    for (int t = 0; t < n_writers; ++t) {
        readers.emplace_back([&h]() {
            for (int i = 0; i < per_thread; ++i) {
                (void)h.recent(10);
                (void)h.size();
            }
        });
    }

    for (auto& w : writers) { w.join(); }
    for (auto& r : readers) { r.join(); }

    REQUIRE(h.size() == 100);
}

TEST_CASE("test_wrap_around_correctness", "[tool_call_history]") {
    ToolCallHistory h(3);
    // Fill: 0, 1, 2
    for (size_t i = 0; i < 3; ++i) {
        h.record(make_record(i));
    }
    // Overwrite oldest: 3 overwrites 0, 4 overwrites 1
    h.record(make_record(3));
    h.record(make_record(4));

    auto r = h.recent(3);
    REQUIRE(r.size() == 3);
    REQUIRE(r[0].sequence == 4);  // newest
    REQUIRE(r[1].sequence == 3);
    REQUIRE(r[2].sequence == 2);  // oldest surviving

    auto a = h.all();
    REQUIRE(a.size() == 3);
    REQUIRE(a[0].sequence == 2);  // oldest surviving
    REQUIRE(a[1].sequence == 3);
    REQUIRE(a[2].sequence == 4);  // newest
}
