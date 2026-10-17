// SPDX-License-Identifier: Apache-2.0
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
 * @req REQ-TYPE-006
 * @version 1.9.11
 */
struct Message {
    std::string role;                                          ///< Message role
    std::string content;                                       ///< Message text content (always populated)
    std::vector<ContentPart> content_parts;                    ///< Multimodal parts (empty for text-only)
    std::unordered_map<std::string, std::string> metadata;     ///< Arbitrary metadata
};

/**
 * @brief Metadata keys that carry tool use through the prompt (v2.14.6).
 *
 * The engine stores a turn's tool calls and each result's call id on the
 * messages' metadata; the prompt serializer sends them as fields beside
 * role/content, and the backend renders the chat template's native
 * tool-call and tool-response turns from them. Before v2.14.6 only the
 * cleaned text travelled, so the model never saw its own calls.
 *
 * @version 2.14.6
 */
namespace tool_meta {
inline constexpr const char* kCalls = "tool_calls";          ///< Assistant: JSON array [{id,name,arguments}]
inline constexpr const char* kCallText = "tool_call_text";   ///< Assistant: the raw call text the model wrote
inline constexpr const char* kCallId = "tool_call_id";       ///< Result: the call it answers
inline constexpr const char* kName = "tool_name";            ///< Result: the tool that produced it
} // namespace tool_meta

} // namespace entropic
