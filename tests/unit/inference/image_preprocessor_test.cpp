/**
 * @file test_image_preprocessor.cpp
 * @brief Tests for ImagePreprocessor validation and resize.
 * @version 1.9.11
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/image_preprocessor.h>

#include <filesystem>
#include <fstream>

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

/**
 * @brief Get path to the test image fixture.
 * @return Path to 100x100 test PNG.
 * @utility
 * @version 1.9.11
 */
static std::filesystem::path test_image() {
    return std::filesystem::path(TEST_DATA_DIR) / "test_image_100x100.png";
}

// ── Valid image processing ──────────────────────────────────

TEST_CASE("Preprocess valid small image", "[preprocessor]") {
    entropic::ImagePreprocessConfig config;
    entropic::ImagePreprocessor pp(config);

    auto img = pp.preprocess_file(test_image());

    REQUIRE(img.width == 100);
    REQUIRE(img.height == 100);
    REQUIRE(img.channels == 3);
    REQUIRE_FALSE(img.pixel_data.empty());
    REQUIRE(img.source_path == test_image().string());
}

TEST_CASE("Image within bounds is not resized", "[preprocessor]") {
    entropic::ImagePreprocessConfig config;
    config.max_width = 1280;
    config.max_height = 1280;
    entropic::ImagePreprocessor pp(config);

    auto img = pp.preprocess_file(test_image());

    REQUIRE(img.width == 100);
    REQUIRE(img.height == 100);
}

// ── File validation ─────────────────────────────────────────

TEST_CASE("Reject non-existent file", "[preprocessor]") {
    entropic::ImagePreprocessConfig config;
    entropic::ImagePreprocessor pp(config);

    REQUIRE_THROWS_AS(
        pp.preprocess_file("/nonexistent/path.png"),
        std::runtime_error);
}

TEST_CASE("Reject file exceeding max size", "[preprocessor]") {
    entropic::ImagePreprocessConfig config;
    config.max_file_size = 1; // 1 byte — any real file exceeds this
    entropic::ImagePreprocessor pp(config);

    REQUIRE_THROWS_AS(
        pp.preprocess_file(test_image()),
        std::runtime_error);
}

TEST_CASE("Reject non-image file", "[preprocessor]") {
    entropic::ImagePreprocessConfig config;
    entropic::ImagePreprocessor pp(config);

    // Create a temporary text file
    auto tmp = std::filesystem::temp_directory_path() / "not_an_image.txt";
    {
        std::ofstream f(tmp);
        f << "this is not an image";
    }

    REQUIRE_THROWS_AS(pp.preprocess_file(tmp), std::runtime_error);
    std::filesystem::remove(tmp);
}

// ── Buffer processing ───────────────────────────────────────

TEST_CASE("Preprocess from memory buffer", "[preprocessor]") {
    entropic::ImagePreprocessConfig config;
    entropic::ImagePreprocessor pp(config);

    // Read test image into memory
    auto path = test_image();
    auto size = std::filesystem::file_size(path);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    std::ifstream f(path, std::ios::binary);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(size));

    auto img = pp.preprocess_buffer(buf.data(), buf.size(), "test_buf");

    REQUIRE(img.width == 100);
    REQUIRE(img.height == 100);
    REQUIRE(img.channels == 3);
    REQUIRE(img.source_path == "test_buf");
}

// ── Resize ──────────────────────────────────────────────────

TEST_CASE("Resize triggers when image exceeds max dims",
          "[preprocessor]") {
    entropic::ImagePreprocessConfig config;
    config.max_width = 50;
    config.max_height = 50;
    entropic::ImagePreprocessor pp(config);

    auto img = pp.preprocess_file(test_image());

    REQUIRE(img.width <= 50);
    REQUIRE(img.height <= 50);
    REQUIRE_FALSE(img.pixel_data.empty());
}

TEST_CASE("Resize preserves aspect ratio", "[preprocessor]") {
    entropic::ImagePreprocessConfig config;
    config.max_width = 50;
    config.max_height = 80;
    config.preserve_aspect = true;
    entropic::ImagePreprocessor pp(config);

    // 100x100 → both need to fit in 50x80 → scale=0.5 → 50x50
    auto img = pp.preprocess_file(test_image());

    REQUIRE(img.width == 50);
    REQUIRE(img.height == 50);
}
