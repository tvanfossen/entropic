// SPDX-License-Identifier: Apache-2.0
/**
 * @file utf8_safe_substr_test.cpp
 * @brief gh#56 (v2.2.3) regression: history previews must truncate on UTF-8
 *        codepoint boundaries.
 *
 * Pre-fix behavior: `sp_get_history` truncated each message preview via
 * `m.content.substr(0, 200) + "..."`. Byte-indexed truncation slices through
 * multi-byte UTF-8 codepoints whose bytes straddle the 200-byte boundary,
 * producing invalid UTF-8 at the seam. The subsequent `arr.dump()` raised
 * `nlohmann::json::type_error.316`, bubbling out `run_streaming` as
 * `ENTROPIC_ERROR_GENERATE_FAILED`.
 *
 * Post-fix behavior: `entropic::facade::utf8_safe_substr` walks back over
 * any UTF-8 continuation bytes (`0x80..0xBF`) before the cut, so the result
 * is always valid UTF-8 when the input is. The fixed `sp_get_history`
 * therefore returns a JSON array that `nlohmann::json::parse` accepts.
 *
 * @version 2.2.3
 */

#include "utf8_safe.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

using entropic::facade::utf8_safe_substr;

TEST_CASE("utf8_safe_substr returns input when shorter than cap",
          "[utf8][facade]") {
    REQUIRE(utf8_safe_substr("hello", 200) == "hello");
    REQUIRE(utf8_safe_substr("", 200) == "");
    REQUIRE(utf8_safe_substr(std::string(200, 'a'), 200) ==
            std::string(200, 'a'));
}

TEST_CASE("utf8_safe_substr cuts cleanly on ASCII boundary",
          "[utf8][facade]") {
    std::string s(300, 'a');
    auto out = utf8_safe_substr(s, 200);
    REQUIRE(out.size() == 200);
    REQUIRE(out == std::string(200, 'a'));
}

TEST_CASE("utf8_safe_substr never slices a multi-byte codepoint",
          "[utf8][facade][gh56]") {
    // 199 ASCII 'a' bytes, then 5 copies of U+00E9 (LATIN SMALL LETTER E
    // WITH ACUTE — 2 bytes each: 0xC3 0xA9). The 200-byte cap lands on the
    // continuation byte 0xA9 of the first 'é'. Naive substr would emit that
    // 0xA9 with no leading 0xC3 → invalid UTF-8 → json::type_error.316.
    std::string s(199, 'a');
    for (int i = 0; i < 5; ++i) {
        s += "\xC3\xA9";
    }
    REQUIRE(s.size() == 199 + 5 * 2);

    auto out = utf8_safe_substr(s, 200);
    // The cut must round DOWN to the codepoint boundary at byte 199 — the
    // start of the first 'é' — so the prefix is the 199 'a' bytes only.
    REQUIRE(out.size() == 199);
    REQUIRE(out == std::string(199, 'a'));

    // The fixed prefix concatenated with "..." parses as a JSON string.
    nlohmann::json j = out + "...";
    REQUIRE(j.is_string());
    REQUIRE_NOTHROW(j.dump());
}

TEST_CASE("utf8_safe_substr keeps a fully-contained codepoint",
          "[utf8][facade]") {
    // Cap at 201 — the entire first 'é' fits, so the cut must include both
    // of its bytes (0xC3 0xA9) and stop before the second 'é'.
    std::string s(199, 'a');
    for (int i = 0; i < 5; ++i) {
        s += "\xC3\xA9";
    }
    auto out = utf8_safe_substr(s, 201);
    REQUIRE(out.size() == 201);
    REQUIRE(out.substr(199) == std::string("\xC3\xA9", 2));
}

TEST_CASE("utf8_safe_substr handles 3-byte and 4-byte UTF-8 sequences",
          "[utf8][facade]") {
    // U+FFFD REPLACEMENT CHARACTER — 3 bytes: 0xEF 0xBF 0xBD.
    // gh#56 repro pattern: docs.db built from non-UTF-8 source files
    // contains U+FFFD runs.
    std::string s(198, 'a');
    for (int i = 0; i < 4; ++i) {
        s += "\xEF\xBF\xBD";
    }
    // Cap at 200 lands on the second byte (0xBF) of the first U+FFFD —
    // the cut must round back to byte 198.
    auto out = utf8_safe_substr(s, 200);
    REQUIRE(out.size() == 198);
    REQUIRE(out == std::string(198, 'a'));
    REQUIRE_NOTHROW(nlohmann::json(out + "...").dump());

    // U+1F600 GRINNING FACE — 4 bytes: 0xF0 0x9F 0x98 0x80. Cap mid-sequence.
    std::string t(197, 'a');
    t += "\xF0\x9F\x98\x80";
    auto out2 = utf8_safe_substr(t, 199);
    REQUIRE(out2.size() == 197);
    REQUIRE_NOTHROW(nlohmann::json(out2 + "...").dump());
}
