/**
 * @file content.cpp
 * @brief Multimodal content type implementations.
 * @version 1.9.11
 */

#include <entropic/types/content.h>

namespace entropic {

/**
 * @brief Extract concatenated text from content parts.
 * @param parts Content parts array.
 * @return All text parts joined with spaces, images skipped.
 *         Empty string if parts is empty or contains no text.
 * @internal
 * @version 1.9.11
 */
std::string extract_text(const std::vector<ContentPart>& parts) {
    std::string result;
    for (const auto& part : parts) {
        if (part.type != ContentPartType::TEXT) {
            continue;
        }
        if (!result.empty()) {
            result += ' ';
        }
        result += part.text;
    }
    return result;
}

/**
 * @brief Check if content parts contain any image parts.
 * @param parts Content parts array.
 * @return true if at least one IMAGE part exists.
 * @internal
 * @version 1.9.11
 */
bool has_images(const std::vector<ContentPart>& parts) {
    for (const auto& part : parts) {
        if (part.type == ContentPartType::IMAGE) {
            return true;
        }
    }
    return false;
}

} // namespace entropic
