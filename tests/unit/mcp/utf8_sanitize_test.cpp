// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file utf8_sanitize_test.cpp
 * @brief Coverage for entropic::mcp::sanitize_utf8 (v2.1.0 #47).
 *
 * The sanitizer is a small RFC-3629-compliant UTF-8 validator that
 * replaces malformed sequences with U+FFFD. Tests cover the four
 * categories that matter for the bug it solves:
 *   1. Pass-through: ASCII and well-formed multi-byte input untouched.
 *   2. Replacement: malformed bytes / overlong / surrogate / truncated.
 *   3. Boundary cases: start, end, repeated invalid runs.
 *   4. JSON-roundtrip: nlohmann::json::dump() succeeds on the result.
 *
 * @version 2.1.0
 */

#include <entropic/mcp/utf8_sanitize.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

using entropic::mcp::sanitize_utf8;

constexpr const char* kReplacement = "\xEF\xBF\xBD";  // U+FFFD

SCENARIO("sanitize_utf8 passes ASCII through unchanged",
         "[mcp][utf8][2.1.0]")
{
    GIVEN("a pure ASCII string") {
        const std::string in = "hello, world! 0123 ~`!@#$%^&*()_+";
        WHEN("sanitized") {
            auto out = sanitize_utf8(in);
            THEN("output equals input") { CHECK(out == in); }
        }
    }
}

SCENARIO("sanitize_utf8 passes well-formed multi-byte UTF-8 through",
         "[mcp][utf8][2.1.0]")
{
    GIVEN("strings spanning the four valid UTF-8 byte-widths") {
        // 1-byte: ASCII; 2-byte: ä; 3-byte: 中; 4-byte: 𝄞 (musical symbol)
        const std::string in = "ascii ä 中 \xF0\x9D\x84\x9E end";
        WHEN("sanitized") {
            auto out = sanitize_utf8(in);
            THEN("output equals input") { CHECK(out == in); }
        }
    }
}

SCENARIO("sanitize_utf8 replaces lone continuation bytes with U+FFFD",
         "[mcp][utf8][2.1.0]")
{
    GIVEN("a string with a stray continuation byte (0x80) between ASCII") {
        const std::string in = std::string("a") + '\x80' + "b";
        WHEN("sanitized") {
            auto out = sanitize_utf8(in);
            THEN("the stray byte is replaced with U+FFFD") {
                CHECK(out == std::string("a") + kReplacement + "b");
            }
        }
    }
}

SCENARIO("sanitize_utf8 replaces truncated multi-byte sequences",
         "[mcp][utf8][2.1.0]")
{
    GIVEN("a 3-byte sequence cut short to 2 bytes at end of input") {
        // 0xE4 0xB8 needs a third continuation byte for U+4E00..; missing.
        const std::string in = std::string("X") + '\xE4' + '\xB8';
        WHEN("sanitized") {
            auto out = sanitize_utf8(in);
            THEN("truncated bytes each become a replacement char") {
                // Robust resync: lead byte invalid → 1 replacement,
                // continuation byte then validated standalone → another.
                CHECK(out == std::string("X") + kReplacement + kReplacement);
            }
        }
    }
}

SCENARIO("sanitize_utf8 rejects overlong encodings",
         "[mcp][utf8][2.1.0]")
{
    GIVEN("an overlong 2-byte form of NUL (0xC0 0x80, illegal per RFC 3629)") {
        const std::string in = std::string("a") + '\xC0' + '\x80' + "b";
        WHEN("sanitized") {
            auto out = sanitize_utf8(in);
            THEN("the overlong sequence is replaced") {
                // 0xC0 → invalid lead → replace+advance 1.
                // 0x80 → stray continuation → replace+advance 1.
                CHECK(out == std::string("a") + kReplacement + kReplacement + "b");
            }
        }
    }
}

SCENARIO("sanitize_utf8 rejects UTF-16 surrogate codepoints",
         "[mcp][utf8][2.1.0]")
{
    GIVEN("a surrogate-half encoded as 3-byte UTF-8 (0xED 0xA0 0x80, U+D800)") {
        const std::string in = std::string("X") + '\xED' + '\xA0' + '\x80';
        WHEN("sanitized") {
            auto out = sanitize_utf8(in);
            THEN("the surrogate sequence is replaced") {
                CHECK(out.find(kReplacement) != std::string::npos);
                CHECK(out.find('\xED') == std::string::npos);
            }
        }
    }
}

SCENARIO("sanitize_utf8 output round-trips through nlohmann::json::dump",
         "[mcp][utf8][2.1.0]")
{
    GIVEN("a payload mixing valid UTF-8, ASCII, and a malformed byte") {
        const std::string in =
            "valid ä 中 then \xC3\x28 then valid";  // 0xC3 0x28 is invalid
        WHEN("sanitized and wrapped in a JSON object") {
            auto out = sanitize_utf8(in);
            nlohmann::json obj = {{"content", out}};
            THEN("json::dump succeeds without type_error 316") {
                REQUIRE_NOTHROW(obj.dump());
            }
            AND_THEN("the dump round-trips through parse") {
                auto parsed = nlohmann::json::parse(obj.dump());
                CHECK(parsed["content"].get<std::string>() == out);
            }
        }
    }
}

SCENARIO("sanitize_utf8 handles empty input",
         "[mcp][utf8][2.1.0]")
{
    GIVEN("an empty string") {
        WHEN("sanitized") {
            auto out = sanitize_utf8("");
            THEN("output is empty") { CHECK(out.empty()); }
        }
    }
}
