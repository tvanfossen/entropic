// SPDX-License-Identifier: Apache-2.0
/**
 * @file messages_json.cpp
 * @brief Implementation of the shared messages-JSON parser.
 * @version 2.1.8
 */

#include <entropic/types/messages_json.h>
#include <entropic/types/content.h>

#include <nlohmann/json.hpp>

namespace entropic {

namespace {

/**
 * @brief Parse a single content-part JSON object → ContentPart.
 * @param part JSON object with "type" and content fields.
 * @return Parsed ContentPart (TEXT if type is anything other than "image").
 * @internal
 * @version 2.1.8
 */
ContentPart parse_content_part(const nlohmann::json& part) {
    ContentPart cp;
    auto type_str = part.value("type", "text");
    if (type_str == "image") {
        cp.type = ContentPartType::IMAGE;
        cp.image_path = part.value("path", "");
        cp.image_url = part.value("url", "");
    } else {
        cp.type = ContentPartType::TEXT;
        cp.text = part.value("text", "");
    }
    return cp;
}

/**
 * @brief Populate a Message from a JSON object.
 * @param m JSON message object.
 * @return Filled Message struct.
 * @internal
 * @version 2.1.8
 */
Message parse_one_message(const nlohmann::json& m) {
    Message msg;
    msg.role = m.value("role", "user");
    if (m.contains("content") && m["content"].is_array()) {
        for (const auto& part : m["content"]) {
            msg.content_parts.push_back(parse_content_part(part));
        }
        msg.content = extract_text(msg.content_parts);
    } else {
        msg.content = m.value("content", "");
    }
    return msg;
}

} // namespace

/**
 * @brief Parse a messages-array JSON string into Message structs.
 * @param json_str Null-terminated JSON. NULL or non-array yields empty.
 * @return Parsed messages (empty on null/non-array input).
 * @internal
 * @version 2.1.8
 */
std::vector<Message> parse_messages_json(const char* json_str) {
    std::vector<Message> messages;
    if (json_str == nullptr) { return messages; }
    auto arr = nlohmann::json::parse(json_str);
    if (!arr.is_array()) { return messages; }
    for (const auto& m : arr) {
        messages.push_back(parse_one_message(m));
    }
    return messages;
}

/**
 * @brief True if any parsed message carries image content_parts.
 * @param messages Parsed message list.
 * @return true if at least one ContentPart is IMAGE.
 * @internal
 * @version 2.1.8
 */
bool any_message_has_images(const std::vector<Message>& messages) {
    for (const auto& m : messages) {
        if (has_images(m.content_parts)) { return true; }
    }
    return false;
}

} // namespace entropic
