// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file messages_json_test.cpp
 * @brief Unit tests for the shared messages-JSON parser (gh#37, v2.1.8).
 *
 * Exercises text-only, multimodal, and edge-case shapes. The parser
 * also runs inside `src/inference/inference_c_api.cpp` (since v1.9.11),
 * but those callsites carry their own integration coverage; this file
 * is dedicated to the extracted utility surface.
 *
 * @version 2.1.8
 */

#include <entropic/types/messages_json.h>
#include <entropic/types/content.h>

#include <catch2/catch_test_macros.hpp>

using entropic::any_message_has_images;
using entropic::ContentPartType;
using entropic::parse_messages_json;

SCENARIO("parse_messages_json: text-only string content", "[v2.1.8][messages_json]") {
    GIVEN("a JSON array with a single string-content message") {
        const char* j = R"([{"role":"user","content":"hello"}])";
        WHEN("parsed") {
            auto msgs = parse_messages_json(j);
            THEN("one message with content set and no content_parts") {
                REQUIRE(msgs.size() == 1);
                REQUIRE(msgs[0].role == "user");
                REQUIRE(msgs[0].content == "hello");
                REQUIRE(msgs[0].content_parts.empty());
                REQUIRE_FALSE(any_message_has_images(msgs));
            }
        }
    }
}

SCENARIO("parse_messages_json: multimodal content array", "[v2.1.8][messages_json]") {
    GIVEN("a JSON array with a text+image content array") {
        const char* j = R"([{"role":"user","content":[
            {"type":"text","text":"describe"},
            {"type":"image","path":"/tmp/foo.png"}
        ]}])";
        WHEN("parsed") {
            auto msgs = parse_messages_json(j);
            THEN("content_parts is populated and content is extracted text") {
                REQUIRE(msgs.size() == 1);
                REQUIRE(msgs[0].content_parts.size() == 2);
                REQUIRE(msgs[0].content_parts[0].type == ContentPartType::TEXT);
                REQUIRE(msgs[0].content_parts[0].text == "describe");
                REQUIRE(msgs[0].content_parts[1].type == ContentPartType::IMAGE);
                REQUIRE(msgs[0].content_parts[1].image_path == "/tmp/foo.png");
                REQUIRE(msgs[0].content == "describe");
                REQUIRE(any_message_has_images(msgs));
            }
        }
    }
}

SCENARIO("parse_messages_json: image data URI", "[v2.1.8][messages_json]") {
    GIVEN("a content part using data: URL for the image") {
        const char* j = R"([{"role":"user","content":[
            {"type":"image","url":"data:image/png;base64,iVBORw0=="},
            {"type":"text","text":"what?"}
        ]}])";
        WHEN("parsed") {
            auto msgs = parse_messages_json(j);
            THEN("image_url is preserved verbatim") {
                REQUIRE(msgs[0].content_parts[0].type == ContentPartType::IMAGE);
                REQUIRE(msgs[0].content_parts[0].image_url
                        == "data:image/png;base64,iVBORw0==");
                REQUIRE(msgs[0].content_parts[0].image_path.empty());
            }
        }
    }
}

SCENARIO("parse_messages_json: missing role defaults to user", "[v2.1.8][messages_json]") {
    GIVEN("a message object with no role field") {
        const char* j = R"([{"content":"orphaned"}])";
        WHEN("parsed") {
            auto msgs = parse_messages_json(j);
            THEN("role defaults to user") {
                REQUIRE(msgs.size() == 1);
                REQUIRE(msgs[0].role == "user");
                REQUIRE(msgs[0].content == "orphaned");
            }
        }
    }
}

SCENARIO("parse_messages_json: malformed JSON throws", "[v2.1.8][messages_json]") {
    GIVEN("an unterminated JSON string") {
        const char* j = R"([{"role":"user","content":"hello)";
        WHEN("parsed") {
            THEN("nlohmann parser throws") {
                REQUIRE_THROWS(parse_messages_json(j));
            }
        }
    }
}

SCENARIO("parse_messages_json: NULL input returns empty", "[v2.1.8][messages_json]") {
    GIVEN("a null pointer") {
        WHEN("parsed") {
            auto msgs = parse_messages_json(nullptr);
            THEN("empty vector returned, no throw") {
                REQUIRE(msgs.empty());
            }
        }
    }
}

SCENARIO("any_message_has_images: detects across multiple messages", "[v2.1.8][messages_json]") {
    GIVEN("two text-only messages and one with an image") {
        const char* j = R"([
            {"role":"system","content":"be helpful"},
            {"role":"user","content":[{"type":"text","text":"hi"}]},
            {"role":"user","content":[{"type":"image","path":"/a.png"}]}
        ])";
        WHEN("parsed and checked") {
            auto msgs = parse_messages_json(j);
            THEN("any_message_has_images returns true") {
                REQUIRE(msgs.size() == 3);
                REQUIRE(any_message_has_images(msgs));
            }
        }
    }
}
