// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file image_preprocessor.h
 * @brief Image preprocessing for multimodal inference.
 *
 * Validates, decodes, resizes, and outputs pixel data ready for the
 * vision encoder. Uses stb_image (bundled with llama.cpp) for decoding.
 *
 * @par Thread safety
 * Stateless after construction. Multiple threads can call preprocess
 * methods concurrently on different images.
 *
 * Internal to inference .so.
 *
 * @version 1.9.11
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Image preprocessing configuration.
 * @version 1.9.11
 */
struct ImagePreprocessConfig {
    int max_width = 1280;                       ///< Maximum image width (resized if exceeded)
    int max_height = 1280;                      ///< Maximum image height (resized if exceeded)
    size_t max_file_size = 20 * 1024 * 1024;    ///< Maximum file size in bytes (20MB)
    bool preserve_aspect = true;                ///< Preserve aspect ratio when resizing
};

/**
 * @brief Preprocessed image ready for vision encoder.
 * @version 1.9.11
 */
struct PreprocessedImage {
    std::vector<uint8_t> pixel_data;  ///< RGB pixel data (row-major)
    int width = 0;                    ///< Image width in pixels
    int height = 0;                   ///< Image height in pixels
    int channels = 3;                 ///< Color channels (always 3 = RGB)
    std::string source_path;          ///< Original source path (for logging)
};

/**
 * @brief Image preprocessor — validates, resizes, and normalizes images.
 *
 * Concrete class (not a base class — no expected variants). Handles
 * the pipeline: load -> validate -> resize -> output.
 *
 * @version 1.9.11
 */
class ImagePreprocessor {
public:
    /**
     * @brief Construct preprocessor with config.
     * @param config Preprocessing configuration.
     * @version 1.9.11
     */
    explicit ImagePreprocessor(const ImagePreprocessConfig& config);

    /**
     * @brief Preprocess an image from file path.
     * @param path File path to image.
     * @return Preprocessed image ready for vision encoder.
     * @throws std::runtime_error on invalid format, oversized file, or read error.
     * @version 1.9.11
     */
    PreprocessedImage preprocess_file(const std::filesystem::path& path);

    /**
     * @brief Preprocess an image from memory buffer.
     * @param data Raw image data (JPEG, PNG, BMP, GIF).
     * @param len Data length in bytes.
     * @param source_label Label for logging (e.g., "data_uri").
     * @return Preprocessed image.
     * @throws std::runtime_error on invalid format or decode error.
     * @version 1.9.11
     */
    PreprocessedImage preprocess_buffer(
        const uint8_t* data,
        size_t len,
        const std::string& source_label);

private:
    ImagePreprocessConfig config_;  ///< Preprocessing configuration

    /**
     * @brief Decode raw bytes into a PreprocessedImage.
     * @param data Raw image bytes.
     * @param len Byte count.
     * @param source Label for error messages.
     * @return Decoded image.
     * @version 1.9.11
     */
    PreprocessedImage decode(
        const uint8_t* data, size_t len,
        const std::string& source);

    /**
     * @brief Resize image if it exceeds max dimensions.
     * @param img Image to potentially resize (mutated in place).
     * @version 1.9.11
     */
    void resize_if_needed(PreprocessedImage& img);
};

} // namespace entropic
