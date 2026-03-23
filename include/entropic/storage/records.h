/**
 * @file records.h
 * @brief Typed record structs for storage persistence.
 *
 * ConversationRecord, MessageRecord, DelegationRecord — C++ structs
 * used internally by SqliteStorageBackend. JSON serialization uses
 * nlohmann/json (internal to .so, not in interface headers).
 *
 * @version 1.8.8
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace entropic {

/**
 * @brief Database record for a conversation.
 * @version 1.8.8
 */
struct ConversationRecord {
    std::string id;                           ///< UUID primary key
    std::string title;                        ///< Conversation title
    std::string created_at;                   ///< ISO 8601 timestamp
    std::string updated_at;                   ///< ISO 8601 timestamp
    std::optional<std::string> project_path;  ///< Project path (nullable)
    std::optional<std::string> model_id;      ///< Model identifier (nullable)
    std::string metadata = "{}";              ///< JSON metadata blob
};

/**
 * @brief Database record for a message.
 * @version 1.8.8
 */
struct MessageRecord {
    std::string id;                           ///< UUID primary key
    std::string conversation_id;              ///< Parent conversation FK
    std::string role;                         ///< "user", "assistant", "system", "tool"
    std::string content;                      ///< Message text
    std::string tool_calls = "[]";            ///< JSON array of tool calls
    std::string tool_results = "[]";          ///< JSON array of tool results
    int64_t token_count = 0;                  ///< Estimated token count
    std::string created_at;                   ///< ISO 8601 timestamp
    bool is_compacted = false;                ///< Whether this message survived compaction
    std::optional<std::string> identity_tier; ///< Tier that produced this message
};

/**
 * @brief Database record for a delegation.
 * @version 1.8.8
 */
struct DelegationRecord {
    std::string id;                           ///< UUID primary key
    std::string parent_conversation_id;       ///< Parent conversation FK
    std::string child_conversation_id;        ///< Child conversation FK
    std::string delegating_tier;              ///< Tier that initiated delegation
    std::string target_tier;                  ///< Target tier for child loop
    std::string task;                         ///< Task description
    std::optional<int> max_turns;             ///< Turn limit (nullable)
    std::string status = "pending";           ///< pending/running/completed/failed
    std::optional<std::string> result_summary; ///< Result summary (nullable)
    std::string created_at;                   ///< ISO 8601 timestamp
    std::optional<std::string> completed_at;  ///< Completion timestamp (nullable)
};

/**
 * @brief Generate a UUID v4 string.
 * @return UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000").
 * @utility
 * @version 1.8.8
 */
std::string generate_uuid();

/**
 * @brief Get current UTC time as ISO 8601 string.
 * @return Timestamp string (e.g., "2026-03-23T12:00:00").
 * @utility
 * @version 1.8.8
 */
std::string utc_timestamp();

/**
 * @brief Create a new ConversationRecord with generated UUID and timestamps.
 * @param title Conversation title.
 * @param project_path Project path (optional).
 * @param model_id Model identifier (optional).
 * @return Populated record ready for INSERT.
 * @utility
 * @version 1.8.8
 */
ConversationRecord make_conversation(
    const std::string& title = "New Conversation",
    const std::optional<std::string>& project_path = std::nullopt,
    const std::optional<std::string>& model_id = std::nullopt);

/**
 * @brief Create a new DelegationRecord with generated UUID and timestamp.
 * @param parent_conversation_id Parent conversation ID.
 * @param child_conversation_id Child conversation ID.
 * @param delegating_tier Tier initiating delegation.
 * @param target_tier Target tier for child loop.
 * @param task Task description.
 * @return Populated record ready for INSERT.
 * @utility
 * @version 1.8.8
 */
DelegationRecord make_delegation(
    const std::string& parent_conversation_id,
    const std::string& child_conversation_id,
    const std::string& delegating_tier,
    const std::string& target_tier,
    const std::string& task);

} // namespace entropic
