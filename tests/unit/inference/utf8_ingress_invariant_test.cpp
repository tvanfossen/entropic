// SPDX-License-Identifier: Apache-2.0
/**
 * @file utf8_ingress_invariant_test.cpp
 * @brief gh#136: model bytes are sanitized at INGRESS, so no downstream
 *        `.dump()` can throw type_error.316.
 *
 * @par Why this exists — four recurrences of one bug
 * `json.exception.type_error.316` has been "fixed" four times:
 *
 *   gh#112 / gh#113 — closed as "permanent closure of the 316 family"
 *   gh#114          — sanitize the delegation task field
 *   gh#118          — sanitize tool-call argument strings
 *   gh#132          — sanitize CompleteTool summary + 2 facade sites
 *   gh#136          — recurred again on v2.10.2
 *
 * Every one patched a single EGRESS site — one `.dump()` among the ~18 in the
 * facade alone, plus more in core. That strategy cannot converge: the exits
 * keep multiplying, and instance N+1 always lands on a site nobody had reached
 * yet. gh#136's reporter proposed patching yet another egress (the facade
 * boundary) which was in fact ALREADY sanitized, so it would not have caught
 * their crash.
 *
 * The entries do not multiply. Model bytes become a std::string at a small,
 * enumerable set of points; everything downstream copies an in-memory string.
 * Sanitizing there makes every dump safe BY CONSTRUCTION, and this test pins
 * the property rather than any particular call site — so a new `.dump()`
 * anywhere cannot reintroduce the bug.
 *
 * @par What must hold
 * `sanitize_utf8` must render any byte sequence safe for `nlohmann::json` to
 * serialize, while leaving valid text byte-identical (or the fix would corrupt
 * every response to prevent a rare one).
 *
 * @version 2.10.4
 */

#include <entropic/mcp/utf8_sanitize.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace {

/// The exact shape gh#136 reported: a multi-byte codepoint truncated by a
/// length/budget boundary, leaving a lead byte with no continuation.
std::string truncated_codepoint() {
    std::string s = "The answer is ";
    s += static_cast<char>(0xE2);  // lead byte of a 3-byte sequence (—)
    s += static_cast<char>(0x80);  // one continuation...
    s += '.';                      // ...then 0x2E, exactly as reported
    return s;
}

/**
 * @brief Serialize through the same path the engine uses.
 * @param content Content to place in a JSON object and dump.
 * @return true when dump() succeeded.
 * @version 2.10.4
 */
bool dumps_cleanly(const std::string& content) {
    try {
        nlohmann::json j;
        j["content"] = content;
        (void)j.dump();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

SCENARIO("gh#136 sanitized model output is always serializable",
         "[inference][utf8][gh136][invariant]")
{
    GIVEN("the truncated-codepoint shape from the v2.10.2 report") {
        auto raw = truncated_codepoint();

        THEN("the raw bytes DO break dump() — the control that proves this "
             "test can fail") {
            // If this ever passes, the fixture stopped reproducing the bug and
            // every assertion below becomes vacuous.
            CHECK_FALSE(dumps_cleanly(raw));
        }

        THEN("sanitized, it serializes") {
            CHECK(dumps_cleanly(entropic::mcp::sanitize_utf8(raw)));
        }

        THEN("the readable text survives — a fix that destroyed content "
             "would be worse than the crash") {
            auto clean = entropic::mcp::sanitize_utf8(raw);
            CHECK(clean.find("The answer is") != std::string::npos);
        }
    }

    GIVEN("assorted malformed sequences a truncated decode can produce") {
        const std::string cases[] = {
            std::string("\xE2\x80"),          // truncated 3-byte
            std::string("\xF0\x9F\x98"),      // truncated 4-byte (emoji)
            std::string("\xC3"),              // lone 2-byte lead
            std::string("\x80\x80"),          // orphan continuations
            std::string("ok\xFF\xFEbad"),     // invalid bytes mid-string
        };
        THEN("every one serializes after sanitize") {
            for (const auto& c : cases) {
                INFO("case bytes=" << c.size());
                CHECK(dumps_cleanly(entropic::mcp::sanitize_utf8(c)));
            }
        }
    }

    GIVEN("valid text, including multi-byte and emoji") {
        const std::string cases[] = {
            "plain ascii",
            "em—dash and \xC3\xA9 accents",
            "emoji \xF0\x9F\x98\x80 intact",
            "",
        };
        THEN("sanitize is byte-identical — it must not touch good output") {
            for (const auto& c : cases) {
                CHECK(entropic::mcp::sanitize_utf8(c) == c);
            }
        }
    }
}
