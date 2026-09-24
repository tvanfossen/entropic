// SPDX-License-Identifier: Apache-2.0
/**
 * @file messages_json.cpp
 * @brief Implementation of the shared messages-JSON parser.
 * @version 2.1.8
 */

#include <entropic/types/messages_json.h>
#include <entropic/types/content.h>

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>

namespace entropic {

namespace {

/**
 * @brief Parse a single content-part JSON object → ContentPart.
 * @param part JSON object with "type" and content fields.
 * @return Parsed ContentPart (TEXT if type is anything other than "image").
 * @dg_internal
 * @version 2.13.0
 */
ContentPart parse_content_part(const nlohmann::json& part) {
    ContentPart cp;
    auto type_str = part.value("type", "text");
    if (type_str == "image") {
        cp.type = ContentPartType::IMAGE;
        cp.image_path = part.value("path", "");
        cp.image_url = part.value("url", "");
        // gh#165 (v2.13.0): geometry is set by the image preprocessor, so a
        // restore that dropped it would silently re-run preprocessing on a
        // conversation that had already paid for it. Absent = 0 = "not yet
        // processed", which is the struct's own default.
        cp.width = part.value("width", 0);
        cp.height = part.value("height", 0);
    } else {
        cp.type = ContentPartType::TEXT;
        cp.text = part.value("text", "");
    }
    return cp;
}

/**
 * @brief Read a message's metadata object, if present (gh#165).
 *
 * Values are read as strings because `Message::metadata` is
 * `unordered_map<string,string>` — a number or bool in the wire form is
 * dumped back through nlohmann so the round trip stays total rather than
 * throwing on a consumer-authored payload.
 *
 * @param m JSON message object.
 * @param[out] out Metadata map to fill.
 * @dg_internal
 * @version 2.13.0
 */
void parse_metadata(const nlohmann::json& m,
                    std::unordered_map<std::string, std::string>& out) {
    if (!m.contains("metadata") || !m["metadata"].is_object()) { return; }
    for (const auto& [k, v] : m["metadata"].items()) {
        out[k] = v.is_string() ? v.get<std::string>() : v.dump();
    }
}

/**
 * @brief Populate a Message from a JSON object.
 * @param m JSON message object.
 * @return Filled Message struct.
 * @dg_internal
 * @version 2.13.0
 */
Message parse_one_message(const nlohmann::json& m) {
    Message msg;
    msg.role = m.value("role", "user");
    if (m.contains("content") && m["content"].is_array()) {
        // The INBOUND multimodal shape (entropic_run_messages): parts live
        // in `content` and the text is derived from them.
        for (const auto& part : m["content"]) {
            msg.content_parts.push_back(parse_content_part(part));
        }
        msg.content = extract_text(msg.content_parts);
    } else {
        msg.content = m.value("content", "");
        // gh#165 (v2.13.0): the OUTBOUND shape `serialize_messages` emits —
        // `content` stays a string (so a consumer reading it is unaffected)
        // and parts ride alongside. Reading both here is what closes the
        // get -> set -> get round trip.
        if (m.contains("content_parts") && m["content_parts"].is_array()) {
            for (const auto& part : m["content_parts"]) {
                msg.content_parts.push_back(parse_content_part(part));
            }
        }
    }
    parse_metadata(m, msg.metadata);
    return msg;
}

} // namespace

/**
 * @brief Parse a messages-array JSON string into Message structs.
 * @param json_str Null-terminated JSON. NULL or non-array yields empty.
 * @return Parsed messages (empty on null/non-array input).
 * @req REQ-TYPE-006
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
 * @req REQ-TYPE-006
 * @version 2.1.8
 */
bool any_message_has_images(const std::vector<Message>& messages) {
    for (const auto& m : messages) {
        if (has_images(m.content_parts)) { return true; }
    }
    return false;
}

} // namespace entropic
