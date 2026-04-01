/**
 * @file message.h
 * @brief Message struct for conversation history.
 *
 * Internal C++ representation. At .so boundaries, messages are serialized
 * as JSON arrays: [{"role":"user","content":"...","metadata":{...},"tool_calls":[]}]
 *
 * @version 1.9.11
 */

#pragma once

#include <entropic/types/content.h>

#include <string>
#include <vector>
#include <unordered_map>

namespace entropic {

/**
 * @brief A message in a conversation.
 *
 * Maps to Python Message dataclass. Roles: "user", "assistant", "system", "tool".
 * Metadata is arbitrary key-value pairs (not enforced at type level).
 *
 * For text-only messages, content is populated and content_parts is empty.
 * For multimodal messages, content_parts is populated and content holds
 * the extracted text (for backward compatibility with code that reads
 * content directly).
 *
 * @version 1.9.11
 */
struct Message {
    std::string role;                                          ///< Message role
    std::string content;                                       ///< Message text content (always populated)
    std::vector<ContentPart> content_parts;                    ///< Multimodal parts (empty for text-only)
    std::unordered_map<std::string, std::string> metadata;     ///< Arbitrary metadata
};

} // namespace entropic
