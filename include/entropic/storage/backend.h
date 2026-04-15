// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file backend.h
 * @brief SqliteStorageBackend — conversation persistence via SQLite.
 *
 * Internal to librentropic-storage.so. Wraps SqliteDatabase with
 * higher-level operations: conversation CRUD, delegation storage,
 * full-text search, compaction snapshots, and statistics.
 *
 * @version 1.8.8
 */

#pragma once

#include <entropic/storage/database.h>
#include <entropic/storage/records.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief SQLite-based storage backend.
 *
 * Provides conversation persistence, message storage, delegation
 * record management, full-text search, and compaction snapshots.
 * Thread-safe via SqliteDatabase's internal mutex.
 *
 * @par Lifecycle:
 * @code
 *   SqliteStorageBackend storage("/path/to/entropic.db");
 *   storage.initialize();
 *   auto id = storage.create_conversation("Title");
 *   storage.save_messages(id, messages_json);
 *   storage.close();
 * @endcode
 *
 * @version 1.8.8
 */
class SqliteStorageBackend {
public:
    /**
     * @brief Construct with database file path.
     * @param db_path Path to SQLite database file.
     * @version 1.8.8
     */
    explicit SqliteStorageBackend(const std::filesystem::path& db_path);

    /**
     * @brief Initialize storage (open database, run migrations).
     * @return true on success.
     * @version 1.8.8
     */
    bool initialize();

    /**
     * @brief Close storage and database connection.
     * @version 1.8.8
     */
    void close();

    // ── Conversation CRUD ─────────────────────────────────

    /**
     * @brief Create a new conversation.
     * @param title Conversation title.
     * @param project_path Project path (optional).
     * @param model_id Model identifier (optional).
     * @return Conversation ID (UUID string).
     * @version 1.8.8
     */
    std::string create_conversation(
        const std::string& title = "New Conversation",
        const std::optional<std::string>& project_path = std::nullopt,
        const std::optional<std::string>& model_id = std::nullopt);

    /**
     * @brief Save messages to a conversation.
     * @param conversation_id Conversation ID.
     * @param messages_json JSON array of message objects.
     * @return true on success.
     * @version 1.8.8
     */
    bool save_messages(const std::string& conversation_id,
                       const std::string& messages_json);

    /**
     * @brief Load a conversation with messages.
     * @param conversation_id Conversation ID.
     * @param[out] result_json JSON with "conversation" and "messages".
     * @return true if found.
     * @version 1.8.8
     */
    bool load_conversation(const std::string& conversation_id,
                           std::string& result_json);

    /**
     * @brief List conversations with pagination.
     * @param limit Maximum results.
     * @param offset Pagination offset.
     * @param[out] result_json JSON array of conversation summaries.
     * @return true on success.
     * @version 1.8.8
     */
    bool list_conversations(int limit, int offset,
                            std::string& result_json);

    /**
     * @brief Delete a conversation and all associated records.
     * @param conversation_id Conversation ID.
     * @return true on success.
     * @version 1.8.8
     */
    bool delete_conversation(const std::string& conversation_id);

    /**
     * @brief Update a conversation's title.
     * @param conversation_id Conversation ID.
     * @param title New title.
     * @return true on success.
     * @version 1.8.8
     */
    bool update_title(const std::string& conversation_id,
                      const std::string& title);

    // ── Search ────────────────────────────────────────────

    /**
     * @brief Full-text search across conversations.
     * @param query FTS5 query string.
     * @param limit Maximum results.
     * @param[out] result_json JSON array of search results with snippets.
     * @return true on success.
     * @version 1.8.8
     */
    bool search_conversations(const std::string& query, int limit,
                              std::string& result_json);

    // ── Delegation storage ────────────────────────────────

    /**
     * @brief Create a delegation record with a child conversation.
     * @param parent_conversation_id Parent conversation ID.
     * @param delegating_tier Tier initiating delegation.
     * @param target_tier Target tier for child loop.
     * @param task Task description.
     * @param max_turns Max turns for child (0 = unlimited).
     * @param[out] delegation_id Created delegation ID.
     * @param[out] child_conversation_id Created child conversation ID.
     * @return true on success.
     * @version 1.8.8
     */
    bool create_delegation(
        const std::string& parent_conversation_id,
        const std::string& delegating_tier,
        const std::string& target_tier,
        const std::string& task,
        int max_turns,
        std::string& delegation_id,
        std::string& child_conversation_id);

    /**
     * @brief Mark a delegation as completed or failed.
     * @param delegation_id Delegation ID.
     * @param status "completed" or "failed".
     * @param result_summary Summary text (optional).
     * @return true on success.
     * @version 1.8.8
     */
    bool complete_delegation(
        const std::string& delegation_id,
        const std::string& status,
        const std::optional<std::string>& result_summary = std::nullopt);

    /**
     * @brief Get delegations for a parent conversation.
     * @param conversation_id Parent conversation ID.
     * @param[out] result_json JSON array of delegation records.
     * @return true on success.
     * @version 1.8.8
     */
    bool get_delegations(const std::string& conversation_id,
                         std::string& result_json);

    // ── Compaction snapshots ──────────────────────────────

    /**
     * @brief Save a pre-compaction snapshot of full conversation history.
     * @param conversation_id Conversation ID.
     * @param messages_json JSON array of all messages before compaction.
     * @return true on success.
     * @version 1.8.8
     */
    bool save_snapshot(const std::string& conversation_id,
                       const std::string& messages_json);

    // ── Statistics ────────────────────────────────────────

    /**
     * @brief Get storage statistics.
     * @param[out] result_json JSON with total_conversations, total_messages, total_tokens.
     * @return true on success.
     * @version 1.8.8
     */
    bool get_stats(std::string& result_json);

private:
    SqliteDatabase db_; ///< Underlying database connection
};

} // namespace entropic
