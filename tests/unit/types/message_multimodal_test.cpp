// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_message_multimodal.cpp
 * @brief Tests for Message struct multimodal content_parts extension.
 * @version 1.9.11
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/types/message.h>

using entropic::ContentPart;
using entropic::ContentPartType;
using entropic::Message;

// ── Message struct backward compatibility ───────────────────

TEST_CASE("Text-only message has empty content_parts", "[message]") {
    Message msg;
    msg.role = "user";
    msg.content = "hello";

    REQUIRE(msg.content == "hello");
    REQUIRE(msg.content_parts.empty());
}

TEST_CASE("Default-constructed Message has empty content_parts",
          "[message]") {
    Message msg;
    REQUIRE(msg.content_parts.empty());
    REQUIRE(msg.content.empty());
    REQUIRE(msg.role.empty());
}

// ── Multimodal Message construction ─────────────────────────

TEST_CASE("Message with content_parts populated", "[message]") {
    Message msg;
    msg.role = "user";

    ContentPart text_part;
    text_part.type = ContentPartType::TEXT;
    text_part.text = "describe this";
    msg.content_parts.push_back(text_part);

    ContentPart img_part;
    img_part.type = ContentPartType::IMAGE;
    img_part.image_path = "/tmp/img.png";
    msg.content_parts.push_back(img_part);

    msg.content = entropic::extract_text(msg.content_parts);

    REQUIRE(msg.content == "describe this");
    REQUIRE(msg.content_parts.size() == 2);
    REQUIRE(msg.content_parts[0].type == ContentPartType::TEXT);
    REQUIRE(msg.content_parts[1].type == ContentPartType::IMAGE);
}

TEST_CASE("Message content_parts with data URI", "[message]") {
    Message msg;
    msg.role = "user";

    ContentPart img;
    img.type = ContentPartType::IMAGE;
    img.image_url = "data:image/png;base64,abc123";
    msg.content_parts.push_back(img);

    ContentPart text;
    text.type = ContentPartType::TEXT;
    text.text = "what is this?";
    msg.content_parts.push_back(text);

    msg.content = entropic::extract_text(msg.content_parts);

    REQUIRE(msg.content == "what is this?");
    REQUIRE(msg.content_parts[0].image_url ==
            "data:image/png;base64,abc123");
}

// ── Metadata unaffected ─────────────────────────────────────

TEST_CASE("Multimodal message preserves metadata", "[message]") {
    Message msg;
    msg.role = "user";
    msg.content = "test";
    msg.metadata["source"] = "user";

    ContentPart img;
    img.type = ContentPartType::IMAGE;
    img.image_path = "/tmp/x.png";
    msg.content_parts.push_back(img);

    REQUIRE(msg.metadata["source"] == "user");
    REQUIRE(msg.content_parts.size() == 1);
}
