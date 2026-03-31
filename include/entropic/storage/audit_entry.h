/**
 * @file audit_entry.h
 * @brief AuditEntry struct and JSON serialization for JSONL audit log.
 *
 * Each AuditEntry represents one tool call execution. The struct carries
 * per-call data; session-level fields (version, timestamp, session_id,
 * sequence) are added by AuditLogger when writing to disk.
 *
 * @version 1.9.5
 */

#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace entropic {

/**
 * @brief A single audit log entry for one MCP tool call.
 *
 * Carries the per-call payload. The AuditLogger wraps this with
 * session-level metadata (version, timestamp, session_id, sequence)
 * when serializing to JSONL.
 *
 * @version 1.9.5
 */
struct AuditEntry {
    std::string caller_id;               ///< Identity/tier name (e.g., "eng", "qa", "lead")
    std::string tool_name;               ///< Fully-qualified tool name (e.g., "filesystem.write_file")
    std::string params_json;             ///< Tool parameters as JSON string (never truncated)
    std::string result_status;           ///< "success", "error", or "timeout"
    std::string result_content;          ///< Tool result text (full, never truncated)
    double elapsed_ms = 0.0;            ///< Tool execution duration in milliseconds
    std::string directives_json;         ///< Directives array as JSON string ("[]" if none)
    int delegation_depth = 0;            ///< Current delegation depth (0 = lead)
    int iteration = 0;                   ///< Engine loop iteration number
    std::string parent_conversation_id;  ///< Parent conversation ID (empty for lead)
};

/**
 * @brief Serialize AuditEntry fields to a JSON object.
 *
 * Produces the per-call payload portion of a JSONL line.
 * params_json and directives_json are parsed into proper JSON
 * objects/arrays (not double-encoded strings).
 *
 * @param entry The audit entry to serialize.
 * @return JSON object with all entry fields.
 * @version 1.9.5
 */
nlohmann::json audit_entry_to_json(const AuditEntry& entry);

/**
 * @brief Deserialize AuditEntry fields from a JSON object.
 *
 * Reads the per-call payload from a JSONL line. Session-level
 * fields (version, timestamp, session_id, sequence) are ignored.
 * Unknown fields are silently skipped.
 *
 * @param j JSON object (one line from audit.jsonl).
 * @param[out] entry Populated entry.
 * @return true if all required fields were present and valid.
 * @version 1.9.5
 */
bool audit_entry_from_json(const nlohmann::json& j, AuditEntry& entry);

} // namespace entropic
