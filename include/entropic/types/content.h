/**
 * @file content.h
 * @brief Multimodal content types for messages.
 *
 * Defines ContentPart and ContentPartType for representing mixed
 * text/image message content. Included by message.h — keep deps
 * minimal (string, vector, cstdint only).
 *
 * @version 1.9.11
 */

#pragma once

#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Content part type discriminant.
 * @version 1.9.11
 */
enum class ContentPartType {
    TEXT,   ///< Plain text content
    IMAGE,  ///< Image content (local path or data URI)
};

/**
 * @brief A single content part within a multimodal message.
 *
 * Messages with simple string content are represented as a single
 * TEXT part. Multimodal messages have multiple parts. The engine
 * preserves part ordering — image position relative to text matters
 * for some models.
 *
 * @version 1.9.11
 */
struct ContentPart {
    ContentPartType type;       ///< Part type discriminant
    std::string text;           ///< Text content (type == TEXT)
    std::string image_path;     ///< Local file path (type == IMAGE)
    std::string image_url;      ///< URL or data URI (type == IMAGE)
    int width = 0;              ///< Image width after preprocessing (0 = not yet processed)
    int height = 0;             ///< Image height after preprocessing (0 = not yet processed)
};

/**
 * @brief Extract concatenated text from content parts.
 * @param parts Content parts array.
 * @return All text parts joined with spaces, images skipped.
 * @version 1.9.11
 */
std::string extract_text(const std::vector<ContentPart>& parts);

/**
 * @brief Check if content parts contain any image parts.
 * @param parts Content parts array.
 * @return true if at least one IMAGE part exists.
 * @version 1.9.11
 */
bool has_images(const std::vector<ContentPart>& parts);

} // namespace entropic
