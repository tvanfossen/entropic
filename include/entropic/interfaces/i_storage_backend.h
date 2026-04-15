// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file i_storage_backend.h
 * @brief Pure C interface contract for storage backends.
 *
 * Defines the .so boundary for librentropic-storage. All functions
 * accept/return C types only (opaque handles, const char*, error codes).
 * No C++ types cross this boundary.
 *
 * Storage is fully optional. NULL handle checks are the consumer's
 * responsibility — passing NULL to any function returns an error code.
 *
 * @version 1.8.8
 */

#pragma once

#include <entropic/types/error.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque handle to a storage backend instance.
typedef struct entropic_storage_backend* entropic_storage_backend_t;

// ── Lifecycle ─────────────────────────────────────────────

/**
 * @brief Create a storage backend with SQLite at the given path.
 * @param db_path Path to SQLite database file. Created if absent.
 * @return Storage handle, or NULL on failure.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_storage_backend_t
entropic_storage_create(const char* db_path);

/**
 * @brief Initialize storage (run migrations, create tables).
 * @param storage Storage handle.
 * @return ENTROPIC_OK on success.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_initialize(entropic_storage_backend_t storage);

/**
 * @brief Destroy storage and close connections.
 * @param storage Storage handle (NULL-safe).
 * @version 1.8.8
 */
ENTROPIC_EXPORT void
entropic_storage_destroy(entropic_storage_backend_t storage);

// ── Conversation CRUD ─────────────────────────────────────

/**
 * @brief Create a new conversation.
 * @param storage Storage handle.
 * @param title Conversation title (null-terminated).
 * @param project_path Project path or NULL.
 * @param model_id Model identifier or NULL.
 * @return Conversation ID string. Caller must free with entropic_free().
 *         NULL on failure.
 * @version 1.8.8
 */
ENTROPIC_EXPORT char*
entropic_storage_create_conversation(
    entropic_storage_backend_t storage,
    const char* title,
    const char* project_path,
    const char* model_id);

/**
 * @brief Save messages to a conversation.
 * @param storage Storage handle.
 * @param conversation_id Conversation ID.
 * @param messages_json JSON array of message objects.
 * @return ENTROPIC_OK on success.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_save_conversation(
    entropic_storage_backend_t storage,
    const char* conversation_id,
    const char* messages_json);

/**
 * @brief Load a conversation with messages.
 * @param storage Storage handle.
 * @param conversation_id Conversation ID.
 * @param result_json Output: JSON with "conversation" and "messages".
 *        Caller must free with entropic_free().
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_INVALID_ARGUMENT if not found.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_load_conversation(
    entropic_storage_backend_t storage,
    const char* conversation_id,
    char** result_json);

/**
 * @brief List conversations with pagination.
 * @param storage Storage handle.
 * @param limit Max results.
 * @param offset Pagination offset.
 * @param result_json Output: JSON array of conversation summaries.
 *        Caller must free with entropic_free().
 * @return ENTROPIC_OK on success.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_list_conversations(
    entropic_storage_backend_t storage,
    int limit,
    int offset,
    char** result_json);

/**
 * @brief Full-text search across conversations.
 * @param storage Storage handle.
 * @param query FTS5 query string.
 * @param limit Max results.
 * @param result_json Output: JSON array of search results with snippets.
 *        Caller must free with entropic_free().
 * @return ENTROPIC_OK on success.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_search_conversations(
    entropic_storage_backend_t storage,
    const char* query,
    int limit,
    char** result_json);

/**
 * @brief Delete a conversation and all associated records.
 * @param storage Storage handle.
 * @param conversation_id Conversation ID.
 * @return ENTROPIC_OK on success.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_delete_conversation(
    entropic_storage_backend_t storage,
    const char* conversation_id);

// ── Delegation storage ────────────────────────────────────

/**
 * @brief Create a delegation record with a child conversation.
 * @param storage Storage handle.
 * @param parent_conversation_id Parent conversation ID.
 * @param delegating_tier Tier initiating delegation.
 * @param target_tier Target tier for child loop.
 * @param task Task description.
 * @param max_turns Max turns for child (0 for unlimited).
 * @param result_json Output: JSON with "delegation_id" and
 *        "child_conversation_id". Caller must free.
 * @return ENTROPIC_OK on success.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_create_delegation(
    entropic_storage_backend_t storage,
    const char* parent_conversation_id,
    const char* delegating_tier,
    const char* target_tier,
    const char* task,
    int max_turns,
    char** result_json);

/**
 * @brief Mark a delegation as completed or failed.
 * @param storage Storage handle.
 * @param delegation_id Delegation ID.
 * @param status "completed" or "failed".
 * @param result_summary Summary text or NULL.
 * @return ENTROPIC_OK on success.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_complete_delegation(
    entropic_storage_backend_t storage,
    const char* delegation_id,
    const char* status,
    const char* result_summary);

/**
 * @brief Get delegations for a parent conversation.
 * @param storage Storage handle.
 * @param conversation_id Parent conversation ID.
 * @param result_json Output: JSON array of delegation records.
 *        Caller must free.
 * @return ENTROPIC_OK on success.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_get_delegations(
    entropic_storage_backend_t storage,
    const char* conversation_id,
    char** result_json);

// ── Compaction snapshots ──────────────────────────────────

/**
 * @brief Save a pre-compaction snapshot of full conversation history.
 * @param storage Storage handle.
 * @param conversation_id Conversation ID.
 * @param messages_json JSON array of all messages before compaction.
 * @return ENTROPIC_OK on success.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_save_snapshot(
    entropic_storage_backend_t storage,
    const char* conversation_id,
    const char* messages_json);

// ── Statistics ────────────────────────────────────────────

/**
 * @brief Get storage statistics.
 * @param storage Storage handle.
 * @param result_json Output: JSON with total_conversations,
 *        total_messages, total_tokens. Caller must free.
 * @return ENTROPIC_OK on success.
 * @version 1.8.8
 */
ENTROPIC_EXPORT entropic_error_t
entropic_storage_get_stats(
    entropic_storage_backend_t storage,
    char** result_json);

#ifdef __cplusplus
}
#endif
