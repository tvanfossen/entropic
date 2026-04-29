// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file json_serializers.h
 * @brief JSON serialization helpers for the facade.
 *
 * Concentrates all nlohmann/json usage in one private facade header.
 * The main entropic.cpp should not include nlohmann/json.hpp directly.
 *
 * @version 2.0.1
 */

#pragma once

#include <entropic/mcp/utf8_sanitize.h>
#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace facade_json {

/**
 * @brief Serialize messages to JSON array string.
 *
 * Sanitizes ``content`` through ``entropic::mcp::sanitize_utf8`` before
 * each push so that any bytes which slipped past the inbound sanitize
 * boundaries (model-stream desync, pre-2.1.1 audit-replayed history,
 * pre-2.1.1 stored conversation state) cannot reach ``arr.dump()`` and
 * raise ``json::type_error 316``. This is the C-API outbound boundary;
 * see ``include/entropic/mcp/utf8_sanitize.h`` for the full boundary
 * policy. Issue #3 (v2.1.1).
 *
 * @param messages Message vector.
 * @return JSON array string.
 * @utility
 * @version 2.1.1
 */
inline std::string serialize_messages(
    const std::vector<entropic::Message>& messages) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : messages) {
        arr.push_back({{"role", m.role},
                       {"content", entropic::mcp::sanitize_utf8(m.content)}});
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
