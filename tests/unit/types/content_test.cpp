// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_content.cpp
 * @brief Tests for ContentPart types, extract_text, has_images.
 * @version 1.9.11
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/types/content.h>

using entropic::ContentPart;
using entropic::ContentPartType;
using entropic::extract_text;
using entropic::has_images;

// ── ContentPart construction ────────────────────────────────

TEST_CASE("ContentPart TEXT has correct type", "[content]") {
    ContentPart part;
    part.type = ContentPartType::TEXT;
    part.text = "hello";

    REQUIRE(part.type == ContentPartType::TEXT);
    REQUIRE(part.text == "hello");
    REQUIRE(part.image_path.empty());
    REQUIRE(part.image_url.empty());
}

TEST_CASE("ContentPart IMAGE with path", "[content]") {
    ContentPart part;
    part.type = ContentPartType::IMAGE;
    part.image_path = "/tmp/img.png";

    REQUIRE(part.type == ContentPartType::IMAGE);
    REQUIRE(part.image_path == "/tmp/img.png");
}

TEST_CASE("ContentPart IMAGE with data URI", "[content]") {
    ContentPart part;
    part.type = ContentPartType::IMAGE;
    part.image_url = "data:image/png;base64,abc123";

    REQUIRE(part.type == ContentPartType::IMAGE);
    REQUIRE(part.image_url == "data:image/png;base64,abc123");
}

TEST_CASE("ContentPart default dimensions are zero", "[content]") {
    ContentPart part;
    part.type = ContentPartType::IMAGE;
    REQUIRE(part.width == 0);
    REQUIRE(part.height == 0);
}

// ── extract_text ────────────────────────────────────────────

TEST_CASE("extract_text from mixed parts", "[content]") {
    std::vector<ContentPart> parts;

    ContentPart t1;
    t1.type = ContentPartType::TEXT;
    t1.text = "hello";
    parts.push_back(t1);

    ContentPart img;
    img.type = ContentPartType::IMAGE;
    img.image_path = "/tmp/a.png";
    parts.push_back(img);

    ContentPart t2;
    t2.type = ContentPartType::TEXT;
    t2.text = "world";
    parts.push_back(t2);

    REQUIRE(extract_text(parts) == "hello world");
}

TEST_CASE("extract_text from text-only parts", "[content]") {
    std::vector<ContentPart> parts;

    ContentPart t1;
    t1.type = ContentPartType::TEXT;
    t1.text = "only text";
    parts.push_back(t1);

    REQUIRE(extract_text(parts) == "only text");
}

TEST_CASE("extract_text from empty parts", "[content]") {
    std::vector<ContentPart> parts;
    REQUIRE(extract_text(parts).empty());
}

TEST_CASE("extract_text from images only", "[content]") {
    std::vector<ContentPart> parts;

    ContentPart img;
    img.type = ContentPartType::IMAGE;
    img.image_path = "/tmp/a.png";
    parts.push_back(img);

    REQUIRE(extract_text(parts).empty());
}

// ── has_images ──────────────────────────────────────────────

TEST_CASE("has_images detects images", "[content]") {
    std::vector<ContentPart> parts;

    ContentPart t1;
    t1.type = ContentPartType::TEXT;
    t1.text = "hi";
    parts.push_back(t1);

    ContentPart img;
    img.type = ContentPartType::IMAGE;
    img.image_path = "/tmp/a.png";
    parts.push_back(img);

    REQUIRE(has_images(parts));
}

TEST_CASE("has_images returns false for text-only", "[content]") {
    std::vector<ContentPart> parts;

    ContentPart t1;
    t1.type = ContentPartType::TEXT;
    t1.text = "hi";
    parts.push_back(t1);

    ContentPart t2;
    t2.type = ContentPartType::TEXT;
    t2.text = "there";
    parts.push_back(t2);

    REQUIRE_FALSE(has_images(parts));
}

TEST_CASE("has_images returns false for empty", "[content]") {
    std::vector<ContentPart> parts;
    REQUIRE_FALSE(has_images(parts));
}
