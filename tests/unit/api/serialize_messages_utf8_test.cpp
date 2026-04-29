// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file serialize_messages_utf8_test.cpp
 * @brief Issue #3 (v2.1.1) regression: facade message serialization must
 *        sanitize UTF-8 at the C-API outbound boundary.
 *
 * v2.1.0 sanitized only at the inbound MCP tool-result boundary on the
 * assumption that "sanitize once, trust downstream" was sufficient. The
 * demo session falsified that: invalid UTF-8 reached
 * ``facade_json::serialize_messages`` (called from ``entropic_run``) via
 * paths that bypassed the inbound sanitizer (model token stream, history
 * replay, sub-engine bridge), producing ``json::type_error 316`` from
 * ``arr.dump()`` and aborting the run.
 *
 * v2.1.1 generalizes to a boundary policy: sanitize at every system
 * seam where bytes change ownership. This test pins the C-API outbound
 * boundary by feeding ``serialize_messages`` Messages whose ``content``
 * carries deliberately malformed bytes and asserting that the function
 * returns parseable JSON instead of throwing.
 *
 * @version 2.1.1
 */

#include "json_serializers.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using entropic::Message;

namespace {

/**
 * @brief Build a Message with the given role and raw byte content.
 *
 * Uses the (pointer, size) string constructor so that NUL bytes and
 * malformed UTF-8 sequences are preserved verbatim — the std::string
 * literal constructor would stop at the first NUL.
 *
 * @param role Message role (e.g. "assistant").
 * @param bytes Raw byte buffer.
 * @param len Length in bytes.
 * @return Constructed Message.
 * @internal
 * @version 2.1.1
 */
Message make_msg(const std::string& role, const char* bytes, size_t len) {
    Message m;
    m.role = role;
    m.content.assign(bytes, len);
    return m;
}

} // namespace

SCENARIO("serialize_messages does not throw type_error 316 on malformed UTF-8",
         "[facade][utf8][regression][2.1.1]") {
    GIVEN("a message containing a lone continuation byte (0xC3 0x28)") {
        // 0xC3 starts a 2-byte sequence; 0x28 ('(') is not a valid
        // continuation byte (must be 0x80..0xBF). Pre-2.1.1 this would
        // pass through to arr.dump() and throw json::type_error 316.
        // The byte 0x28 is the same one cited in the demo session error
        // message (`invalid UTF-8 byte at index 200: 0x2E` — the parser
        // reports the byte at the failed index, not the offending lead
        // byte, so the message is misleading; here we use 0x28 to exercise
        // the same class of malformed run).
        const char bad_bytes[] = {
            'p', 'r', 'e', '-', '\xC3', '\x28', '-', 'p', 'o', 's', 't'
        };
        std::vector<Message> messages;
        messages.push_back(make_msg("assistant", bad_bytes, sizeof(bad_bytes)));

        WHEN("serialize_messages is called") {
            std::string out;
            REQUIRE_NOTHROW(out = facade_json::serialize_messages(messages));

            THEN("the output is parseable JSON") {
                nlohmann::json parsed;
                REQUIRE_NOTHROW(parsed = nlohmann::json::parse(out));
                REQUIRE(parsed.is_array());
                REQUIRE(parsed.size() == 1);
                REQUIRE(parsed[0].at("role").get<std::string>()
                        == "assistant");
            }
            AND_THEN("the malformed byte is replaced with U+FFFD (0xEF 0xBF 0xBD)") {
                auto parsed = nlohmann::json::parse(out);
                auto content = parsed[0].at("content").get<std::string>();
                // U+FFFD = 0xEF 0xBF 0xBD; "pre-" is preserved, the bad
                // 0xC3 lead becomes U+FFFD, the 0x28 ('(') resyncs as
                // valid ASCII, and "-post" follows.
                REQUIRE(content.find("\xEF\xBF\xBD") != std::string::npos);
                REQUIRE(content.find("pre-") != std::string::npos);
                REQUIRE(content.find("-post") != std::string::npos);
            }
        }
    }
}

SCENARIO("serialize_messages preserves valid UTF-8 verbatim",
         "[facade][utf8][regression][2.1.1]") {
    GIVEN("a message containing only well-formed multi-byte UTF-8") {
        // Mix of ASCII, 2-byte (é = 0xC3 0xA9), 3-byte (€ = 0xE2 0x82 0xAC),
        // and 4-byte (𝄞 = 0xF0 0x9D 0x84 0x9E) sequences. Sanitizer must
        // pass these through unchanged.
        const std::string valid = "ASCII é € 𝄞 end";
        std::vector<Message> messages;
        messages.push_back({{"user"}, valid, {}});

        WHEN("serialize_messages is called") {
            auto out = facade_json::serialize_messages(messages);

            THEN("the round-tripped content equals the input exactly") {
                auto parsed = nlohmann::json::parse(out);
                REQUIRE(parsed[0].at("content").get<std::string>() == valid);
            }
        }
    }
}

SCENARIO("serialize_messages handles a mix of valid and malformed messages",
         "[facade][utf8][regression][2.1.1]") {
    GIVEN("a vector containing one clean and one corrupted message") {
        std::vector<Message> messages;
        messages.push_back({{"user"}, "Hello", {}});
        const char bad[] = {'b', 'a', 'd', '\xF0', '\x28'};  // truncated 4-byte
        messages.push_back(make_msg("assistant", bad, sizeof(bad)));

        WHEN("serialize_messages is called") {
            std::string out;
            REQUIRE_NOTHROW(out = facade_json::serialize_messages(messages));

            THEN("both messages serialize and the clean one is unchanged") {
                auto parsed = nlohmann::json::parse(out);
                REQUIRE(parsed.size() == 2);
                REQUIRE(parsed[0].at("content").get<std::string>() == "Hello");
                REQUIRE(parsed[1].at("content").get<std::string>().find(
                            "\xEF\xBF\xBD") != std::string::npos);
            }
        }
    }
}
