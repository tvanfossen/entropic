// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file audit_entry.cpp
 * @brief AuditEntry JSON serialization implementation.
 * @version 1.9.5
 */

#include <entropic/storage/audit_entry.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

static auto logger = entropic::log::get("storage.audit_entry");

namespace entropic {

/**
 * @brief Extract optional parent_conversation_id from JSON.
 * @param j JSON object.
 * @return Conversation ID string, or empty if absent/null.
 * @internal
 * @version 1.9.5
 */
static std::string extract_parent_id(const nlohmann::json& j) {
    if (j.contains("parent_conversation_id") &&
        !j["parent_conversation_id"].is_null()) {
        return j["parent_conversation_id"].get<std::string>();
    }
    return "";
}

/**
 * @brief Parse the result sub-object from JSON into AuditEntry fields.
 * @param result JSON result object.
 * @param[out] entry Entry to populate.
 * @internal
 * @version 1.9.5
 */
static void parse_result_fields(
    const nlohmann::json& result, AuditEntry& entry) {
    entry.result_status = result.at("status").get<std::string>();
    entry.result_content = result.at("content").get<std::string>();
    entry.elapsed_ms = result.at("elapsed_ms").get<double>();
}

/**
 * @brief Serialize AuditEntry fields to a JSON object.
 * @param entry The audit entry to serialize.
 * @return JSON object with all entry fields.
 * @internal
 * @version 1.9.5
 */
nlohmann::json audit_entry_to_json(const AuditEntry& entry) {
    nlohmann::json j;
    j["caller_id"] = entry.caller_id;
    j["tool_name"] = entry.tool_name;
    j["params"] = nlohmann::json::parse(
        entry.params_json.empty() ? "{}" : entry.params_json);
    j["result"]["status"] = entry.result_status;
    j["result"]["content"] = entry.result_content;
    j["result"]["elapsed_ms"] = entry.elapsed_ms;
    j["directives"] = nlohmann::json::parse(
        entry.directives_json.empty() ? "[]" : entry.directives_json);
    j["delegation_depth"] = entry.delegation_depth;
    j["iteration"] = entry.iteration;
    j["parent_conversation_id"] = entry.parent_conversation_id.empty()
        ? nlohmann::json(nullptr)
        : nlohmann::json(entry.parent_conversation_id);
    return j;
}

/**
 * @brief Deserialize AuditEntry fields from a JSON object.
 * @param j JSON object (one JSONL line).
 * @param[out] entry Populated entry.
 * @return true if all required fields present and valid.
 * @internal
 * @version 1.9.5
 */
bool audit_entry_from_json(const nlohmann::json& j, AuditEntry& entry) {
    try {
        entry.caller_id = j.at("caller_id").get<std::string>();
        entry.tool_name = j.at("tool_name").get<std::string>();
        entry.params_json = j.at("params").dump();
        parse_result_fields(j.at("result"), entry);
        entry.directives_json = j.at("directives").dump();
        entry.delegation_depth = j.at("delegation_depth").get<int>();
        entry.iteration = j.at("iteration").get<int>();
        entry.parent_conversation_id = extract_parent_id(j);
        return true;
    } catch (const nlohmann::json::exception& e) {
        logger->warn("Failed to parse audit entry: {}", e.what());
        return false;
    }
}

} // namespace entropic
