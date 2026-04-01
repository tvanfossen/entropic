/**
 * @file image_preprocessor.cpp
 * @brief Image preprocessing implementation.
 *
 * Uses stb_image (vendored with llama.cpp) for decoding. Resize is
 * bilinear downscale implemented in-house (stb_image_resize2 not
 * available in llama.cpp's vendor tree).
 *
 * @version 1.9.11
 */

#include <entropic/inference/image_preprocessor.h>
#include <entropic/types/logging.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>

// stb_image — use llama.cpp's vendored copy.
// STB_IMAGE_STATIC keeps symbols internal to this TU to avoid
// duplicate symbol conflicts with llama.cpp's own stb_image use.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include <stb/stb_image.h>

static auto logger = entropic::log::get("inference.image_preprocessor");

namespace {

/**
 * @brief Bilinear interpolation of one pixel channel.
 * @param data Source RGB pixel buffer.
 * @param w Source image width.
 * @param x0 Left column index.
 * @param y0 Top row index.
 * @param x1 Right column index.
 * @param y1 Bottom row index.
 * @param fx Horizontal interpolation fraction.
 * @param fy Vertical interpolation fraction.
 * @param ch Channel index (0=R, 1=G, 2=B).
 * @return Interpolated pixel value.
 * @utility
 * @version 1.9.11
 */
float lerp_channel(const uint8_t* data, int w,
                   int x0, int y0, int x1, int y1,
                   float fx, float fy, int ch) {
    auto px = [data, w, ch](int x, int y) -> float {
        return static_cast<float>(
            data[(static_cast<size_t>(y) * static_cast<size_t>(w)
                  + static_cast<size_t>(x)) * 3
                 + static_cast<size_t>(ch)]);
    };
    float top = px(x0, y0) * (1.0f - fx) + px(x1, y0) * fx;
    float bot = px(x0, y1) * (1.0f - fx) + px(x1, y1) * fx;
    return top * (1.0f - fy) + bot * fy;
}

/**
 * @brief Interpolate one output pixel (3 channels) into dst.
 * @param src Source pixel buffer.
 * @param src_w Source width.
 * @param src_h Source height.
 * @param dst Destination buffer offset.
 * @param src_x Source x coordinate (float).
 * @param src_y Source y coordinate (float).
 * @utility
 * @version 1.9.11
 */
void interpolate_pixel(const uint8_t* src, int src_w, int src_h,
                       uint8_t* dst,
                       float src_x, float src_y) {
    int x0 = std::max(0, static_cast<int>(src_x));
    int x1 = std::min(src_w - 1, x0 + 1);
    int y0 = std::max(0, static_cast<int>(src_y));
    int y1 = std::min(src_h - 1, y0 + 1);
    float fx = src_x - static_cast<float>(x0);
    float fy = src_y - static_cast<float>(y0);

    for (int ch = 0; ch < 3; ++ch) {
        float val = lerp_channel(src, src_w, x0, y0, x1, y1, fx, fy, ch);
        dst[ch] = static_cast<uint8_t>(std::clamp(val, 0.0f, 255.0f));
    }
}

/**
 * @brief Bilinear interpolation downscale.
 * @param img Source image (mutated with resized data).
 * @param new_w Target width.
 * @param new_h Target height.
 * @utility
 * @version 1.9.11
 */
void bilinear_resize(entropic::PreprocessedImage& img,
                     int new_w, int new_h) {
    std::vector<uint8_t> out(
        static_cast<size_t>(new_w) * static_cast<size_t>(new_h) * 3);

    float x_ratio = static_cast<float>(img.width) / static_cast<float>(new_w);
    float y_ratio = static_cast<float>(img.height) / static_cast<float>(new_h);

    for (int y = 0; y < new_h; ++y) {
        float sy = (static_cast<float>(y) + 0.5f) * y_ratio - 0.5f;
        for (int x = 0; x < new_w; ++x) {
            float sx = (static_cast<float>(x) + 0.5f) * x_ratio - 0.5f;
            size_t dst_off = (static_cast<size_t>(y)
                              * static_cast<size_t>(new_w)
                              + static_cast<size_t>(x)) * 3;
            interpolate_pixel(img.pixel_data.data(),
                              img.width, img.height,
                              out.data() + dst_off, sx, sy);
        }
    }

    img.pixel_data = std::move(out);
    img.width = new_w;
    img.height = new_h;
}

} // anonymous namespace

namespace entropic {

/**
 * @brief Construct preprocessor with config.
 * @param config Preprocessing configuration.
 * @version 1.9.11
 */
ImagePreprocessor::ImagePreprocessor(const ImagePreprocessConfig& config)
    : config_(config) {}

/**
 * @brief Preprocess an image from file path.
 * @param path File path to image.
 * @return Preprocessed image ready for vision encoder.
 * @throws std::runtime_error on invalid format, oversized, or read error.
 * @internal
 * @version 1.9.11
 */
PreprocessedImage ImagePreprocessor::preprocess_file(
    const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        logger->error("Image file not found: {}", path.string());
        throw std::runtime_error(
            "Image file not found: " + path.string());
    }

    auto file_size = std::filesystem::file_size(path);
    if (file_size > config_.max_file_size) {
        logger->error("Image file too large: {} bytes (max {})",
                      file_size, config_.max_file_size);
        throw std::runtime_error(
            "Image exceeds max file size ("
            + std::to_string(file_size) + " > "
            + std::to_string(config_.max_file_size) + ")");
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(
            "Cannot open image file: " + path.string());
    }

    std::vector<uint8_t> buf(static_cast<size_t>(file_size));
    file.read(reinterpret_cast<char*>(buf.data()),
              static_cast<std::streamsize>(file_size));

    auto img = decode(buf.data(), buf.size(), path.string());
    img.source_path = path.string();
    resize_if_needed(img);

    logger->info("Preprocessed image {}: {}x{} ({} bytes)",
                 path.string(), img.width, img.height,
                 img.pixel_data.size());
    return img;
}

/**
 * @brief Preprocess an image from memory buffer.
 * @param data Raw image data (JPEG, PNG, BMP, GIF).
 * @param len Data length in bytes.
 * @param source_label Label for logging (e.g., "data_uri").
 * @return Preprocessed image.
 * @throws std::runtime_error on invalid format or decode error.
 * @internal
 * @version 1.9.11
 */
PreprocessedImage ImagePreprocessor::preprocess_buffer(
    const uint8_t* data,
    size_t len,
    const std::string& source_label) {
    auto img = decode(data, len, source_label);
    img.source_path = source_label;
    resize_if_needed(img);

    logger->info("Preprocessed buffer '{}': {}x{}",
                 source_label, img.width, img.height);
    return img;
}

/**
 * @brief Decode raw bytes into a PreprocessedImage via stb_image.
 * @param data Raw image bytes.
 * @param len Byte count.
 * @param source Label for error messages.
 * @return Decoded image with RGB pixel data.
 * @internal
 * @version 1.9.11
 */
PreprocessedImage ImagePreprocessor::decode(
    const uint8_t* data, size_t len,
    const std::string& source) {
    int w = 0;
    int h = 0;
    int ch = 0;
    auto* pixels = stbi_load_from_memory(
        data, static_cast<int>(len), &w, &h, &ch, 3);

    if (pixels == nullptr) {
        logger->error("Failed to decode image '{}': {}",
                      source, stbi_failure_reason());
        throw std::runtime_error(
            "Unsupported image format or decode error: " + source);
    }

    PreprocessedImage img;
    img.width = w;
    img.height = h;
    img.channels = 3;
    size_t data_size = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    img.pixel_data.assign(pixels, pixels + data_size);
    stbi_image_free(pixels);

    return img;
}

/**
 * @brief Resize image if it exceeds max dimensions.
 *
 * Uses bilinear interpolation for downscaling. Only shrinks, never
 * enlarges. When preserve_aspect is true, both dimensions are scaled
 * uniformly by the smaller of the two scale factors.
 *
 * @param img Image to potentially resize (mutated in place).
 * @internal
 * @version 1.9.11
 */
void ImagePreprocessor::resize_if_needed(PreprocessedImage& img) {
    if (img.width <= config_.max_width
        && img.height <= config_.max_height) {
        return;
    }

    float scale_w = static_cast<float>(config_.max_width)
                  / static_cast<float>(img.width);
    float scale_h = static_cast<float>(config_.max_height)
                  / static_cast<float>(img.height);

    float scale = config_.preserve_aspect
        ? std::min(scale_w, scale_h)
        : 1.0f; // unused, but satisfies compiler

    if (!config_.preserve_aspect) {
        scale_w = std::min(scale_w, 1.0f);
        scale_h = std::min(scale_h, 1.0f);
    } else {
        scale_w = scale;
        scale_h = scale;
    }

    int new_w = std::max(1, static_cast<int>(
        static_cast<float>(img.width) * scale_w));
    int new_h = std::max(1, static_cast<int>(
        static_cast<float>(img.height) * scale_h));

    bilinear_resize(img, new_w, new_h);
}

} // namespace entropic
