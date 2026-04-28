// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file tool_result_classify_test.cpp
 * @brief Coverage for entropic::mcp::is_effectively_empty and
 *        looks_like_tool_error (v2.1.0 #44).
 *
 * The two helpers drive the ToolResultKind classification in
 * tool_executor.cpp:702. Together they let identity prompts teach
 * pivot-on-empty rules and stop error responses from leaking through
 * as ok. Tests cover the four buckets we care about:
 *   1. is_effectively_empty: empty / whitespace-only / non-empty.
 *   2. looks_like_tool_error positive cases: case-insensitive prefix,
 *      bracketed prefix, JSON-shaped error.
 *   3. looks_like_tool_error negative cases: error-text-in-the-middle,
 *      JSON with error key past the byte budget, "errored" word.
 *   4. ToolResultKind::ok_empty serializes to "ok_empty".
 *
 * @version 2.1.0
 */

#include <entropic/mcp/tool_result_classify.h>
#include <entropic/types/tool_result.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using entropic::ToolResultKind;
using entropic::result_kind_to_string;
using entropic::mcp::is_effectively_empty;
using entropic::mcp::looks_like_tool_error;

SCENARIO("is_effectively_empty bucket", "[mcp][tool_result_classify][2.1.0]") {
    GIVEN("various inputs") {
        CHECK(is_effectively_empty(""));
        CHECK(is_effectively_empty(" "));
        CHECK(is_effectively_empty("\t\n\r "));
        CHECK_FALSE(is_effectively_empty("a"));
        CHECK_FALSE(is_effectively_empty(" hello "));
        CHECK_FALSE(is_effectively_empty("(no results)"));
    }
}

SCENARIO("looks_like_tool_error positive cases",
         "[mcp][tool_result_classify][2.1.0]")
{
    GIVEN("rendered tool-error responses") {
        // Plain prefix variants — case-insensitive, with leading
        // whitespace tolerance.
        CHECK(looks_like_tool_error("error: file not found"));
        CHECK(looks_like_tool_error("Error: file not found"));
        CHECK(looks_like_tool_error("ERROR: file not found"));
        CHECK(looks_like_tool_error("\n  Error: file not found"));
        CHECK(looks_like_tool_error("[error] permission denied"));
        CHECK(looks_like_tool_error("[ERROR] permission denied"));
        // JSON-shaped — top-level error key within the first 32 bytes.
        CHECK(looks_like_tool_error(R"({"error":"bad request"})"));
        CHECK(looks_like_tool_error(R"({ "error": "x" })"));
    }
}

SCENARIO("looks_like_tool_error negative cases",
         "[mcp][tool_result_classify][2.1.0]")
{
    GIVEN("content that mentions error but is not an error response") {
        // "errored" prefix matches "error" — that's by design (any
        // error-* word is a strong signal). Document the
        // currently-accepted shape and the explicit non-matches:
        CHECK(looks_like_tool_error("errored at line 5"));   // accepted

        // Mid-string "error" mention — not a leader.
        CHECK_FALSE(looks_like_tool_error("Process completed; one error reported"));

        // JSON with "error" key way past the byte budget — the cheap
        // sniff requires it within the first 32 bytes to keep big
        // payloads from false-positiving.
        std::string padded = R"({"data":")"
            + std::string(64, 'x')
            + R"(","error":"deep"})";
        CHECK_FALSE(looks_like_tool_error(padded));

        // Plain content.
        CHECK_FALSE(looks_like_tool_error("hello, world"));
        CHECK_FALSE(looks_like_tool_error(""));
        CHECK_FALSE(looks_like_tool_error("(no results)"));
    }
}

SCENARIO("ToolResultKind::ok_empty round-trips through string form",
         "[types][tool_result][2.1.0]")
{
    CHECK(std::string(result_kind_to_string(ToolResultKind::ok_empty))
          == "ok_empty");
    // Existing kinds still serialise correctly post-extension.
    CHECK(std::string(result_kind_to_string(ToolResultKind::ok))
          == "ok");
    CHECK(std::string(result_kind_to_string(ToolResultKind::error))
          == "error");
}

// ── Demo ask #6 (v2.1.0): tool result size cap ─────────────────

SCENARIO("truncate_to_cap is a no-op when cap is 0",
         "[mcp][tool_result_classify][2.1.0][demo-ask-6]")
{
    GIVEN("cap=0 and a long input") {
        std::string s(10000, 'x');
        const auto before = s.size();
        WHEN("truncate_to_cap runs with cap=0") {
            entropic::mcp::truncate_to_cap(s, 0);
            THEN("string is unchanged") { CHECK(s.size() == before); }
        }
    }
}

SCENARIO("truncate_to_cap is a no-op when content is already within cap",
         "[mcp][tool_result_classify][2.1.0][demo-ask-6]")
{
    GIVEN("a short string and a generous cap") {
        std::string s = "hello world";
        WHEN("truncate_to_cap runs") {
            entropic::mcp::truncate_to_cap(s, 1024);
            THEN("string is unchanged") { CHECK(s == "hello world"); }
        }
    }

    GIVEN("a string exactly at cap") {
        std::string s(64, 'a');
        WHEN("truncate_to_cap runs at cap=64") {
            entropic::mcp::truncate_to_cap(s, 64);
            THEN("string is unchanged") {
                CHECK(s.size() == 64);
                CHECK(s.find("truncated") == std::string::npos);
            }
        }
    }
}

SCENARIO("truncate_to_cap shrinks oversized content with a marker tail",
         "[mcp][tool_result_classify][2.1.0][demo-ask-6]")
{
    GIVEN("a 10 KB string and a 100-byte cap") {
        std::string s(10000, 'x');
        const auto before = s.size();
        WHEN("truncate_to_cap runs") {
            entropic::mcp::truncate_to_cap(s, 100);
            THEN("final size is at or below cap") {
                CHECK(s.size() <= 100);
            }
            AND_THEN("the truncation marker reports lost bytes") {
                CHECK(s.find("[... truncated,") != std::string::npos);
                CHECK(s.find("more bytes]") != std::string::npos);
                // Lost = before - cap (within tail rounding).
                CHECK(s.find(std::to_string(before - 100))
                      != std::string::npos);
            }
            AND_THEN("kept prefix is the original head") {
                CHECK(s.front() == 'x');
            }
        }
    }
}
