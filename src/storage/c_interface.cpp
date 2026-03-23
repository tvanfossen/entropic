/**
 * @file c_interface.cpp
 * @brief C boundary implementation for i_storage_backend.h.
 *
 * Wraps SqliteStorageBackend in opaque handles, catches all exceptions
 * at the .so boundary, and converts to entropic_error_t codes.
 *
 * @version 1.8.8
 */

#include <entropic/interfaces/i_storage_backend.h>
#include <entropic/storage/backend.h>

#include <spdlog/spdlog.h>

#include <cstring>
#include <new>
#include <optional>
#include <string>

using entropic::SqliteStorageBackend;

// ── Handle mapping ────────────────────────────────────────

/// @internal The opaque handle points directly to a SqliteStorageBackend.
struct entropic_storage_backend {
    SqliteStorageBackend impl;

    /**
     * @brief Construct with database path.
     * @param path Database file path.
     * @internal
     * @version 1.8.8
     */
    explicit entropic_storage_backend(const char* path)
        : impl(path) {}
};

// ── Helpers ───────────────────────────────────────────────

/**
 * @brief Duplicate a std::string to a malloc'd C string.
 * @param s Source string.
 * @return Heap-allocated copy (caller must free with entropic_free).
 * @internal
 * @version 1.8.8
 */
static char* dup_string(const std::string& s) {
    auto* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

/**
 * @brief Convert optional C string to std::optional<std::string>.
 * @param s C string or NULL.
 * @return Optional string.
 * @internal
 * @version 1.8.8
 */
static std::optional<std::string> opt_str(const char* s) {
    return s ? std::optional<std::string>(s) : std::nullopt;
}

// ── Lifecycle ─────────────────────────────────────────────

/**
 * @brief Create a storage backend.
 * @param db_path Database file path.
 * @return Handle or NULL.
 * @internal
 * @version 1.8.8
 */
entropic_storage_backend_t
entropic_storage_create(const char* db_path) {
    if (!db_path) return nullptr;
    try {
        return new entropic_storage_backend(db_path);
    } catch (const std::exception& e) {
        spdlog::error("storage_create failed: {}", e.what());
        return nullptr;
    }
}

/**
 * @brief Initialize storage.
 * @param storage Handle.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t
entropic_storage_initialize(entropic_storage_backend_t storage) {
    if (!storage) return ENTROPIC_ERROR_INVALID_ARGUMENT;
    try {
        return storage->impl.initialize()
            ? ENTROPIC_OK : ENTROPIC_ERROR_IO;
    } catch (const std::exception& e) {
        spdlog::error("storage_initialize failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}

/**
 * @brief Destroy storage.
 * @param storage Handle (NULL-safe).
 * @internal
 * @version 1.8.8
 */
void entropic_storage_destroy(entropic_storage_backend_t storage) {
    delete storage;
}

// ── Conversation CRUD ─────────────────────────────────────

/**
 * @brief Create a conversation.
 * @param storage Handle.
 * @param title Title.
 * @param project_path Optional project path.
 * @param model_id Optional model ID.
 * @return Conversation ID (caller frees) or NULL.
 * @internal
 * @version 1.8.8
 */
char* entropic_storage_create_conversation(
        entropic_storage_backend_t storage,
        const char* title,
        const char* project_path,
        const char* model_id) {
    if (!storage || !title) return nullptr;
    try {
        auto id = storage->impl.create_conversation(
            title, opt_str(project_path), opt_str(model_id));
        return dup_string(id);
    } catch (const std::exception& e) {
        spdlog::error("create_conversation failed: {}", e.what());
        return nullptr;
    }
}

/**
 * @brief Save messages to a conversation.
 * @param storage Handle.
 * @param conversation_id Conversation ID.
 * @param messages_json JSON array.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t entropic_storage_save_conversation(
        entropic_storage_backend_t storage,
        const char* conversation_id,
        const char* messages_json) {
    if (!storage || !conversation_id || !messages_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        return storage->impl.save_messages(conversation_id, messages_json)
            ? ENTROPIC_OK : ENTROPIC_ERROR_IO;
    } catch (const std::exception& e) {
        spdlog::error("save_conversation failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}

/**
 * @brief Load a conversation.
 * @param storage Handle.
 * @param conversation_id Conversation ID.
 * @param result_json Output JSON.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t entropic_storage_load_conversation(
        entropic_storage_backend_t storage,
        const char* conversation_id,
        char** result_json) {
    if (!storage || !conversation_id || !result_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::string json;
        bool found = storage->impl.load_conversation(
            conversation_id, json);
        *result_json = found ? dup_string(json) : nullptr;
        return found ? ENTROPIC_OK : ENTROPIC_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& e) {
        spdlog::error("load_conversation failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}

/**
 * @brief List conversations.
 * @param storage Handle.
 * @param limit Max results.
 * @param offset Offset.
 * @param result_json Output JSON.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t entropic_storage_list_conversations(
        entropic_storage_backend_t storage,
        int limit, int offset,
        char** result_json) {
    if (!storage || !result_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::string json;
        storage->impl.list_conversations(limit, offset, json);
        *result_json = dup_string(json);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        spdlog::error("list_conversations failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}

/**
 * @brief Search conversations.
 * @param storage Handle.
 * @param query FTS5 query.
 * @param limit Max results.
 * @param result_json Output JSON.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t entropic_storage_search_conversations(
        entropic_storage_backend_t storage,
        const char* query, int limit,
        char** result_json) {
    if (!storage || !query || !result_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::string json;
        storage->impl.search_conversations(query, limit, json);
        *result_json = dup_string(json);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        spdlog::error("search_conversations failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}

/**
 * @brief Delete a conversation.
 * @param storage Handle.
 * @param conversation_id Conversation ID.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t entropic_storage_delete_conversation(
        entropic_storage_backend_t storage,
        const char* conversation_id) {
    if (!storage || !conversation_id) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        return storage->impl.delete_conversation(conversation_id)
            ? ENTROPIC_OK : ENTROPIC_ERROR_IO;
    } catch (const std::exception& e) {
        spdlog::error("delete_conversation failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}

// ── Delegation storage ────────────────────────────────────

/**
 * @brief Create a delegation.
 * @param storage Handle.
 * @param parent_conversation_id Parent conversation.
 * @param delegating_tier Source tier.
 * @param target_tier Target tier.
 * @param task Task description.
 * @param max_turns Turn limit.
 * @param result_json Output JSON.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t entropic_storage_create_delegation(
        entropic_storage_backend_t storage,
        const char* parent_conversation_id,
        const char* delegating_tier,
        const char* target_tier,
        const char* task,
        int max_turns,
        char** result_json) {
    if (!storage || !parent_conversation_id || !delegating_tier ||
        !target_tier || !task || !result_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::string del_id, child_id;
        bool ok = storage->impl.create_delegation(
            parent_conversation_id, delegating_tier,
            target_tier, task, max_turns, del_id, child_id);
        if (ok) {
            auto json = "{\"delegation_id\":\"" + del_id +
                        "\",\"child_conversation_id\":\"" + child_id + "\"}";
            *result_json = dup_string(json);
        }
        return ok ? ENTROPIC_OK : ENTROPIC_ERROR_IO;
    } catch (const std::exception& e) {
        spdlog::error("create_delegation failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}

/**
 * @brief Complete a delegation.
 * @param storage Handle.
 * @param delegation_id Delegation ID.
 * @param status Status string.
 * @param result_summary Optional summary.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t entropic_storage_complete_delegation(
        entropic_storage_backend_t storage,
        const char* delegation_id,
        const char* status,
        const char* result_summary) {
    if (!storage || !delegation_id || !status) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        return storage->impl.complete_delegation(
            delegation_id, status, opt_str(result_summary))
            ? ENTROPIC_OK : ENTROPIC_ERROR_IO;
    } catch (const std::exception& e) {
        spdlog::error("complete_delegation failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}

/**
 * @brief Get delegations for a conversation.
 * @param storage Handle.
 * @param conversation_id Parent conversation.
 * @param result_json Output JSON.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t entropic_storage_get_delegations(
        entropic_storage_backend_t storage,
        const char* conversation_id,
        char** result_json) {
    if (!storage || !conversation_id || !result_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::string json;
        storage->impl.get_delegations(conversation_id, json);
        *result_json = dup_string(json);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        spdlog::error("get_delegations failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}

// ── Compaction snapshots ──────────────────────────────────

/**
 * @brief Save a compaction snapshot.
 * @param storage Handle.
 * @param conversation_id Conversation ID.
 * @param messages_json JSON messages.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t entropic_storage_save_snapshot(
        entropic_storage_backend_t storage,
        const char* conversation_id,
        const char* messages_json) {
    if (!storage || !conversation_id || !messages_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        return storage->impl.save_snapshot(conversation_id, messages_json)
            ? ENTROPIC_OK : ENTROPIC_ERROR_IO;
    } catch (const std::exception& e) {
        spdlog::error("save_snapshot failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}

// ── Statistics ────────────────────────────────────────────

/**
 * @brief Get statistics.
 * @param storage Handle.
 * @param result_json Output JSON.
 * @return Error code.
 * @internal
 * @version 1.8.8
 */
entropic_error_t entropic_storage_get_stats(
        entropic_storage_backend_t storage,
        char** result_json) {
    if (!storage || !result_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::string json;
        storage->impl.get_stats(json);
        *result_json = dup_string(json);
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        spdlog::error("get_stats failed: {}", e.what());
        return ENTROPIC_ERROR_IO;
    }
}
