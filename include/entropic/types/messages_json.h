// SPDX-License-Identifier: Apache-2.0
/**
 * @file messages_json.h
 * @brief Shared parser: messages-JSON wire format → vector<Message>.
 *
 * Originally lived in `src/inference/inference_c_api.cpp` as an
 * anonymous-namespace helper (v1.9.11). Extracted to a shared
 * library-internal utility in v2.1.8 so the facade's
 * `entropic_run_messages` entry point can reuse the same parsing
 * without duplicating logic. The implementation depends on
 * nlohmann/json but the header surface is third-party-free per
 * architecture-cpp.md design rule #6.
 *
 * Wire format (OpenAI-compatible content arrays):
 * @code
 * [
 *   {"role":"user","content":"plain text"},
 *   {"role":"user","content":[
 *     {"type":"text","text":"describe"},
 *     {"type":"image","path":"/tmp/foo.png"}
 *   ]}
 * ]
 * @endcode
 *
 * @version 2.1.8
 */
#pragma once

#include <entropic/types/message.h>

#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Parse a JSON array of messages into a vector of Message.
 *
 * Accepts both string-content and content-array (multimodal) shapes.
 * For multimodal messages, populates `Message::content_parts` and
 * sets `Message::content = extract_text(content_parts)` so callers
 * that read `content` directly continue to work.
 *
 * Tolerant of missing fields:
 *   - Missing "role" defaults to "user".
 *   - Missing "content" yields an empty `content` string.
 *
 * @param json_str Null-terminated JSON array string.
 * @return Parsed messages. Empty vector if the array is empty.
 * @throws nlohmann::json::parse_error on malformed JSON.
 * @utility
 * @version 2.1.8
 */
std::vector<Message> parse_messages_json(const char* json_str);

/**
 * @brief Convenience: true if any message carries image content_parts.
 * @param messages Parsed message list.
 * @return true if at least one Message has has_images() true.
 * @utility
 * @version 2.1.8
 */
bool any_message_has_images(const std::vector<Message>& messages);

} // namespace entropic
