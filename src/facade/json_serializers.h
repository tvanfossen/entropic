// SPDX-License-Identifier: Apache-2.0
/**
 * @file json_serializers.h
 * @brief JSON serialization helpers for the facade.
 *
 * Concentrates all nlohmann/json usage in one private facade header.
 * The main entropic.cpp should not include nlohmann/json.hpp directly.
 *
 * @par UTF-8 boundary (issue #3, v2.1.1)
 * Every string is passed through `entropic::mcp::sanitize_utf8` before it is
 * pushed, so bytes that slipped past the inbound sanitize boundaries
 * (model-stream desync, pre-2.1.1 audit-replayed history, pre-2.1.1 stored
 * conversation state) cannot reach `dump()` and raise `json::type_error 316`.
 * This is the C-API OUTBOUND boundary; see
 * `include/entropic/mcp/utf8_sanitize.h` for the full policy.
 *
 * @par Message emission is lossless (gh#165, v2.13.0)
 * `serialize_messages` emitted ONLY role + content until v2.13.0, which made
 * `entropic_session_context_get` a lossy read: `metadata` (every `tool_name`
 * that context_manager, compaction and the engine's tool-result folding key
 * off, and every `is_context_anchor`) and `content_parts` were dropped. That
 * was invisible for as long as nothing fed the output back;
 * `entropic_session_context_set` is the reader that makes it matter — a
 * write counterpart is only worth having if the read it round-trips with
 * preserves what the engine acts on.
 *
 * The addition is ADDITIVE by construction: the new keys appear only when
 * there is something to carry, and `content` stays a STRING on every
 * message, so a consumer reading role/content sees byte-identical output for
 * every message it has ever seen. Parts go in their own `content_parts` key
 * rather than turning `content` into an array (the INBOUND multimodal
 * shape), precisely so that promise holds.
 *
 * @version 2.13.0
 */

#pragma once

#include <entropic/mcp/utf8_sanitize.h>
#include <entropic/types/config.h>
#include <entropic/types/content.h>
#include <entropic/types/message.h>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <string>
#include <vector>

namespace facade_json {

/**
 * @brief Serialize one content part, losslessly (gh#165, v2.13.0).
 *
 * Image geometry is carried because `width`/`height` are set by the
 * preprocessor and a restore that dropped them would silently re-run it.
 *
 * @param p Content part.
 * @return JSON object for that part.
 * @utility
 * @version 2.13.0
 */
inline nlohmann::json serialize_content_part(const entropic::ContentPart& p) {
    nlohmann::json j;
    if (p.type == entropic::ContentPartType::IMAGE) {
        j["type"] = "image";
        j["path"] = entropic::mcp::sanitize_utf8(p.image_path);
        j["url"] = entropic::mcp::sanitize_utf8(p.image_url);
        j["width"] = p.width;
        j["height"] = p.height;
    } else {
        j["type"] = "text";
        j["text"] = entropic::mcp::sanitize_utf8(p.text);
    }
    return j;
}

/**
 * @brief Serialize a message's metadata map, sanitized (gh#165).
 * @param m Message.
 * @return JSON object; empty when the message carries no metadata.
 * @utility
 * @version 2.13.0
 */
inline nlohmann::json serialize_metadata(const entropic::Message& m) {
    nlohmann::json meta = nlohmann::json::object();
    for (const auto& [k, v] : m.metadata) {
        meta[k] = entropic::mcp::sanitize_utf8(v);
    }
    return meta;
}

/**
 * @brief Serialize a message's multimodal parts (gh#165).
 * @param m Message.
 * @return JSON array; empty when the message is text-only.
 * @utility
 * @version 2.13.0
 */
inline nlohmann::json serialize_parts(const entropic::Message& m) {
    nlohmann::json parts = nlohmann::json::array();
    // Indexed rather than a range-for: knots' parser loses this function's
    // closing brace on `for (const auto& p : m.content_parts)` and then
    // charges every following inline in this header to serialize_parts
    // (SLOC 55, ABC 34 for a six-line body). Same class of mis-attribution
    // the comment on LlamaCppBackend::invalidate_resident_kv records. Do not
    // "clean this up" without re-running the complexity gate.
    for (std::size_t i = 0; i < m.content_parts.size(); ++i) {
        parts.push_back(serialize_content_part(m.content_parts[i]));
    }
    return parts;
}

/**
 * @brief Serialize ONE message, losslessly — see the file header (gh#165).
 * @param m Message to serialize.
 * @return JSON object for that message.
 * @req REQ-SAFE-001
 * @req REQ-LOOP-010
 * @version 2.13.0
 */
inline nlohmann::json serialize_message(const entropic::Message& m) {
    nlohmann::json obj = nlohmann::json::object();
    obj["role"] = m.role;
    obj["content"] = entropic::mcp::sanitize_utf8(m.content);
    if (!m.metadata.empty()) { obj["metadata"] = serialize_metadata(m); }
    if (!m.content_parts.empty()) {
        obj["content_parts"] = serialize_parts(m);
    }
    return obj;
}

/**
 * @brief Serialize messages to JSON array string — see serialize_message.
 * @param messages Message vector.
 * @return JSON array string.
 * @req REQ-SAFE-001
 * @req REQ-LOOP-010
 * @version 2.13.0
 */
inline std::string serialize_messages(
    const std::vector<entropic::Message>& messages) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : messages) {
        arr.push_back(serialize_message(m));
    }
    return arr.dump();
}

/**
 * @brief Parse JSON string to nlohmann::json object.
 * @param str JSON string.
 * @return Parsed JSON (discarded on error).
 * @utility
 * @version 2.0.1
 */
inline nlohmann::json parse(const char* str) {
    if (!str) { return nlohmann::json(); }
    return nlohmann::json::parse(str, nullptr, false);
}

/**
 * @brief Create a JSON object from key-value pairs.
 * @return Empty JSON object.
 * @utility
 * @version 2.0.1
 */
inline nlohmann::json obj() { return nlohmann::json::object(); }

/**
 * @brief Create an empty JSON array.
 * @return Empty JSON array.
 * @utility
 * @version 2.0.1
 */
inline nlohmann::json arr() { return nlohmann::json::array(); }

/**
 * @brief Serialize an AdapterInfo to JSON string.
 * @param ai Adapter info struct.
 * @return JSON object string.
 * @utility
 * @version 2.0.1
 */
inline std::string serialize_adapter_info(
    const entropic::AdapterInfo& ai) {
    nlohmann::json j;
    j["name"] = ai.name;
    j["state"] = static_cast<int>(ai.state);
    j["scale"] = ai.scale;
    j["ram_bytes"] = ai.ram_bytes;
    j["path"] = ai.path.string();
    j["tier_name"] = ai.tier_name;
    j["base_model_path"] = ai.base_model_path;
    return j.dump();
}

/**
 * @brief Serialize a list of AdapterInfo to JSON array string.
 * @param adapters Adapter info list.
 * @return JSON array string.
 * @utility
 * @version 2.0.1
 */
inline std::string serialize_adapter_list(
    const std::vector<entropic::AdapterInfo>& adapters) {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& ai : adapters) {
        a.push_back({{"name", ai.name},
                     {"state", static_cast<int>(ai.state)},
                     {"scale", ai.scale},
                     {"tier_name", ai.tier_name}});
    }
    return a.dump();
}

} // namespace facade_json
