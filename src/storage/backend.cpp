/**
 * @file backend.cpp
 * @brief SqliteStorageBackend implementation.
 * @version 1.8.8
 */

#include <entropic/storage/backend.h>

#include <entropic/types/logging.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstring>
#include <random>

using json = nlohmann::json;

namespace entropic {

namespace {
auto logger = entropic::log::get("storage.backend");
} // anonymous namespace

// ── Helpers ───────────────────────────────────────────────

/**
 * @brief Get text from a sqlite3 column, returning empty string for NULL.
 * @param stmt Prepared statement.
 * @param col Column index.
 * @return Column text or empty string.
 * @utility
 * @version 1.8.8
 */
static std::string col_text(sqlite3_stmt* stmt, int col) {
    auto* p = sqlite3_column_text(stmt, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}

/**
 * @brief Get optional text from a sqlite3 column.
 * @param stmt Prepared statement.
 * @param col Column index.
 * @return Column text or nullopt if NULL.
 * @utility
 * @version 1.8.8
 */
static std::optional<std::string> col_opt_text(sqlite3_stmt* stmt, int col) {
    auto* p = sqlite3_column_text(stmt, col);
    if (!p) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(p));
}

/**
 * @brief Bind optional text to a parameter position.
 * @param stmt Prepared statement.
 * @param idx Parameter index (1-based).
 * @param val Value to bind.
 * @utility
 * @version 1.8.8
 */
static void bind_opt_text(sqlite3_stmt* stmt, int idx,
                          const std::optional<std::string>& val) {
    if (val) {
        sqlite3_bind_text(stmt, idx, val->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, idx);
    }
}

// ── SqliteStorageBackend ──────────────────────────────────

/**
 * @brief Construct with database file path.
 * @param db_path Path to SQLite file.
 * @internal
 * @version 1.8.8
 */
SqliteStorageBackend::SqliteStorageBackend(
        const std::filesystem::path& db_path)
    : db_(db_path) {}

/**
 * @brief Initialize storage.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteStorageBackend::initialize() {
    return db_.initialize();
}

/**
 * @brief Close storage.
 * @internal
 * @version 1.8.8
 */
void SqliteStorageBackend::close() {
    db_.close();
}

// ── Conversation CRUD ─────────────────────────────────────

/**
 * @brief Create a new conversation.
 * @param title Conversation title.
 * @param project_path Optional project path.
 * @param model_id Optional model identifier.
 * @return Conversation ID.
 * @internal
 * @version 2.0.0
 */
std::string SqliteStorageBackend::create_conversation(
        const std::string& title,
        const std::optional<std::string>& project_path,
        const std::optional<std::string>& model_id) {
    auto rec = make_conversation(title, project_path, model_id);

    db_.execute(
        "INSERT INTO conversations "
        "(id, title, created_at, updated_at, project_path, model_id, metadata) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, rec.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, rec.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, rec.created_at.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, rec.updated_at.c_str(), -1, SQLITE_TRANSIENT);
            bind_opt_text(s, 5, rec.project_path);
            bind_opt_text(s, 6, rec.model_id);
            sqlite3_bind_text(s, 7, rec.metadata.c_str(), -1, SQLITE_TRANSIENT);
        });

    logger->info("Created conversation: {}", rec.id);
    return rec.id;
}

/**
 * @brief Save messages to a conversation.
 * @param conversation_id Conversation ID.
 * @param messages_json JSON array of message objects.
 * @return true on success.
 * @internal
 * @version 2.0.0
 */
bool SqliteStorageBackend::save_messages(
        const std::string& conversation_id,
        const std::string& messages_json) {
    auto now = utc_timestamp();
    db_.execute(
        "UPDATE conversations SET updated_at = ? WHERE id = ?",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, conversation_id.c_str(), -1, SQLITE_TRANSIENT);
        });

    auto msgs = json::parse(messages_json, nullptr, false);
    if (!msgs.is_array()) {
        logger->error("save_messages: invalid JSON array");
        return false;
    }

    for (const auto& m : msgs) {
        auto msg_id = generate_uuid();
        auto role = m.value("role", "");
        auto content = m.value("content", "");
        auto tool_calls = m.contains("tool_calls") ? m["tool_calls"].dump() : "[]";
        auto tool_results = m.contains("tool_results") ? m["tool_results"].dump() : "[]";
        auto token_count = m.value("token_count", 0);
        auto is_compacted = m.value("is_compacted", false);
        std::optional<std::string> tier;
        if (m.contains("identity_tier") && !m["identity_tier"].is_null()) {
            tier = m["identity_tier"].get<std::string>();
        }

        db_.execute(
            "INSERT INTO messages "
            "(id, conversation_id, role, content, tool_calls, tool_results, "
            "token_count, created_at, is_compacted, identity_tier) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [&](sqlite3_stmt* s) {
                sqlite3_bind_text(s, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, conversation_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, role.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 4, content.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 5, tool_calls.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 6, tool_results.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(s, 7, token_count);
                sqlite3_bind_text(s, 8, now.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(s, 9, is_compacted ? 1 : 0);
                bind_opt_text(s, 10, tier);
            });
    }

    return true;
}

/**
 * @brief Load a conversation with its messages.
 * @param conversation_id Conversation ID.
 * @param[out] result_json JSON result.
 * @return true if found.
 * @internal
 * @version 1.8.8
 */
bool SqliteStorageBackend::load_conversation(
        const std::string& conversation_id,
        std::string& result_json) {
    json conv_obj;
    bool found = db_.fetch_one(
        "SELECT * FROM conversations WHERE id = ?",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, conversation_id.c_str(), -1, SQLITE_TRANSIENT);
        },
        [&](sqlite3_stmt* s) {
            conv_obj["id"] = col_text(s, 0);
            conv_obj["title"] = col_text(s, 1);
            conv_obj["created_at"] = col_text(s, 2);
            conv_obj["updated_at"] = col_text(s, 3);
            conv_obj["project_path"] = col_opt_text(s, 4).value_or("");
            conv_obj["model_id"] = col_opt_text(s, 5).value_or("");
            conv_obj["metadata"] = json::parse(col_text(s, 6), nullptr, false);
        });

    if (!found) return false;

    json messages = json::array();
    db_.fetch_all(
        "SELECT * FROM messages WHERE conversation_id = ? "
        "ORDER BY created_at ASC",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, conversation_id.c_str(), -1, SQLITE_TRANSIENT);
        },
        [&](sqlite3_stmt* s) {
            json msg;
            msg["id"] = col_text(s, 0);
            msg["role"] = col_text(s, 2);
            msg["content"] = col_text(s, 3);
            msg["tool_calls"] = json::parse(col_text(s, 4), nullptr, false);
            msg["tool_results"] = json::parse(col_text(s, 5), nullptr, false);
            msg["token_count"] = sqlite3_column_int64(s, 6);
            msg["created_at"] = col_text(s, 7);
            msg["is_compacted"] = sqlite3_column_int(s, 8) != 0;
            msg["identity_tier"] = col_opt_text(s, 9).value_or("");
            messages.push_back(std::move(msg));
        });

    json result;
    result["conversation"] = std::move(conv_obj);
    result["messages"] = std::move(messages);
    result_json = result.dump();
    return true;
}

/**
 * @brief List conversations with pagination.
 * @param limit Max results.
 * @param offset Pagination offset.
 * @param[out] result_json JSON array of summaries.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteStorageBackend::list_conversations(
        int limit, int offset, std::string& result_json) {
    json arr = json::array();
    db_.fetch_all(
        "SELECT c.id, c.title, c.updated_at, c.project_path, "
        "COUNT(m.id) as message_count "
        "FROM conversations c "
        "LEFT JOIN messages m ON c.id = m.conversation_id "
        "GROUP BY c.id "
        "ORDER BY c.updated_at DESC "
        "LIMIT ? OFFSET ?",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_int(s, 1, limit);
            sqlite3_bind_int(s, 2, offset);
        },
        [&](sqlite3_stmt* s) {
            json entry;
            entry["id"] = col_text(s, 0);
            entry["title"] = col_text(s, 1);
            entry["updated_at"] = col_text(s, 2);
            entry["project_path"] = col_opt_text(s, 3).value_or("");
            entry["message_count"] = sqlite3_column_int(s, 4);
            arr.push_back(std::move(entry));
        });

    result_json = arr.dump();
    return true;
}

/**
 * @brief Delete a conversation and all cascading records.
 * @param conversation_id Conversation ID.
 * @return true on success.
 * @internal
 * @version 2.0.0
 */
bool SqliteStorageBackend::delete_conversation(
        const std::string& conversation_id) {
    bool ok = db_.execute(
        "DELETE FROM conversations WHERE id = ?",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, conversation_id.c_str(), -1, SQLITE_TRANSIENT);
        });
    if (ok) logger->info("Deleted conversation: {}", conversation_id);
    return ok;
}

/**
 * @brief Update a conversation's title.
 * @param conversation_id Conversation ID.
 * @param title New title.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteStorageBackend::update_title(
        const std::string& conversation_id,
        const std::string& title) {
    return db_.execute(
        "UPDATE conversations SET title = ? WHERE id = ?",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, conversation_id.c_str(), -1, SQLITE_TRANSIENT);
        });
}

// ── Search ────────────────────────────────────────────────

/**
 * @brief Full-text search across conversations via FTS5.
 * @param query FTS5 query string.
 * @param limit Max results.
 * @param[out] result_json JSON array of search results.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteStorageBackend::search_conversations(
        const std::string& query, int limit,
        std::string& result_json) {
    json arr = json::array();
    db_.fetch_all(
        "SELECT DISTINCT c.id, c.title, c.updated_at, "
        "snippet(messages_fts, 0, '>>>', '<<<', '...', 32) as snippet "
        "FROM messages_fts "
        "JOIN messages m ON messages_fts.rowid = m.rowid "
        "JOIN conversations c ON m.conversation_id = c.id "
        "WHERE messages_fts MATCH ? "
        "ORDER BY rank "
        "LIMIT ?",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, query.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 2, limit);
        },
        [&](sqlite3_stmt* s) {
            json entry;
            entry["id"] = col_text(s, 0);
            entry["title"] = col_text(s, 1);
            entry["updated_at"] = col_text(s, 2);
            entry["snippet"] = col_text(s, 3);
            arr.push_back(std::move(entry));
        });

    result_json = arr.dump();
    return true;
}

// ── Delegation storage ────────────────────────────────────

/**
 * @brief Create a delegation with a child conversation.
 * @param parent_conversation_id Parent conversation.
 * @param delegating_tier Source tier.
 * @param target_tier Target tier.
 * @param task Task description.
 * @param max_turns Turn limit (0 = unlimited).
 * @param[out] delegation_id Created delegation ID.
 * @param[out] child_conversation_id Created child conversation ID.
 * @return true on success.
 * @internal
 * @version 2.0.0
 */
bool SqliteStorageBackend::create_delegation(
        const std::string& parent_conversation_id,
        const std::string& delegating_tier,
        const std::string& target_tier,
        const std::string& task,
        int max_turns,
        std::string& delegation_id,
        std::string& child_conversation_id) {
    // Create child conversation
    auto child_title = "Delegation: " + target_tier + " — " +
                       task.substr(0, 60);
    child_conversation_id = create_conversation(
        child_title, std::nullopt, target_tier);

    auto rec = make_delegation(
        parent_conversation_id, child_conversation_id,
        delegating_tier, target_tier, task);
    if (max_turns > 0) rec.max_turns = max_turns;
    rec.status = "running";

    bool ok = db_.execute(
        "INSERT INTO delegations "
        "(id, parent_conversation_id, child_conversation_id, "
        "delegating_tier, target_tier, task, max_turns, "
        "status, result_summary, created_at, completed_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, rec.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, rec.parent_conversation_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, rec.child_conversation_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, rec.delegating_tier.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, rec.target_tier.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 6, rec.task.c_str(), -1, SQLITE_TRANSIENT);
            if (rec.max_turns) {
                sqlite3_bind_int(s, 7, *rec.max_turns);
            } else {
                sqlite3_bind_null(s, 7);
            }
            sqlite3_bind_text(s, 8, rec.status.c_str(), -1, SQLITE_TRANSIENT);
            bind_opt_text(s, 9, rec.result_summary);
            sqlite3_bind_text(s, 10, rec.created_at.c_str(), -1, SQLITE_TRANSIENT);
            bind_opt_text(s, 11, rec.completed_at);
        });

    delegation_id = rec.id;
    logger->info("Created delegation {}: {} -> {}",
                 rec.id, delegating_tier, target_tier);
    return ok;
}

/**
 * @brief Mark a delegation as completed or failed.
 * @param delegation_id Delegation ID.
 * @param status "completed" or "failed".
 * @param result_summary Optional summary text.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteStorageBackend::complete_delegation(
        const std::string& delegation_id,
        const std::string& status,
        const std::optional<std::string>& result_summary) {
    auto now = utc_timestamp();
    return db_.execute(
        "UPDATE delegations "
        "SET status = ?, result_summary = ?, completed_at = ? "
        "WHERE id = ?",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, status.c_str(), -1, SQLITE_TRANSIENT);
            bind_opt_text(s, 2, result_summary);
            sqlite3_bind_text(s, 3, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, delegation_id.c_str(), -1, SQLITE_TRANSIENT);
        });
}

/**
 * @brief Get delegations for a parent conversation.
 * @param conversation_id Parent conversation ID.
 * @param[out] result_json JSON array of delegation records.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteStorageBackend::get_delegations(
        const std::string& conversation_id,
        std::string& result_json) {
    json arr = json::array();
    db_.fetch_all(
        "SELECT * FROM delegations "
        "WHERE parent_conversation_id = ? "
        "ORDER BY created_at ASC",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, conversation_id.c_str(), -1, SQLITE_TRANSIENT);
        },
        [&](sqlite3_stmt* s) {
            json entry;
            entry["id"] = col_text(s, 0);
            entry["parent_conversation_id"] = col_text(s, 1);
            entry["child_conversation_id"] = col_text(s, 2);
            entry["delegating_tier"] = col_text(s, 3);
            entry["target_tier"] = col_text(s, 4);
            entry["task"] = col_text(s, 5);
            auto mt = sqlite3_column_int(s, 6);
            entry["max_turns"] = (sqlite3_column_type(s, 6) == SQLITE_NULL)
                                     ? json(nullptr) : json(mt);
            entry["status"] = col_text(s, 7);
            entry["result_summary"] = col_opt_text(s, 8).value_or("");
            entry["created_at"] = col_text(s, 9);
            entry["completed_at"] = col_opt_text(s, 10).value_or("");
            arr.push_back(std::move(entry));
        });

    result_json = arr.dump();
    return true;
}

// ── Compaction snapshots ──────────────────────────────────

/**
 * @brief Save a pre-compaction snapshot.
 * @param conversation_id Conversation ID.
 * @param messages_json JSON array of all messages.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteStorageBackend::save_snapshot(
        const std::string& conversation_id,
        const std::string& messages_json) {
    auto snap_id = generate_uuid();
    auto now = utc_timestamp();

    // Count messages
    auto msgs = json::parse(messages_json, nullptr, false);
    int msg_count = msgs.is_array() ? static_cast<int>(msgs.size()) : 0;

    return db_.execute(
        "INSERT INTO compaction_snapshots "
        "(id, conversation_id, messages_json, message_count, "
        "token_count_estimate, created_at) "
        "VALUES (?, ?, ?, ?, NULL, ?)",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, snap_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, conversation_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, messages_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 4, msg_count);
            sqlite3_bind_text(s, 5, now.c_str(), -1, SQLITE_TRANSIENT);
        });
}

// ── Statistics ────────────────────────────────────────────

/**
 * @brief Get storage statistics.
 * @param[out] result_json JSON with counts.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteStorageBackend::get_stats(std::string& result_json) {
    int64_t total_convs = 0;
    int64_t total_msgs = 0;
    int64_t total_tokens = 0;

    db_.fetch_one("SELECT COUNT(*) FROM conversations",
        nullptr,
        [&](sqlite3_stmt* s) { total_convs = sqlite3_column_int64(s, 0); });

    db_.fetch_one("SELECT COUNT(*) FROM messages",
        nullptr,
        [&](sqlite3_stmt* s) { total_msgs = sqlite3_column_int64(s, 0); });

    db_.fetch_one("SELECT COALESCE(SUM(token_count), 0) FROM messages",
        nullptr,
        [&](sqlite3_stmt* s) { total_tokens = sqlite3_column_int64(s, 0); });

    json stats;
    stats["total_conversations"] = total_convs;
    stats["total_messages"] = total_msgs;
    stats["total_tokens"] = total_tokens;
    result_json = stats.dump();
    return true;
}

// ── Record factory implementations ────────────────────────

/**
 * @brief Generate a UUID v4 string.
 * @return UUID string.
 * @internal
 * @version 1.8.8
 */
std::string generate_uuid() {
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist(0, 15);
    std::uniform_int_distribution<uint32_t> dist2(8, 11);

    const char hex[] = "0123456789abcdef";
    std::string uuid(36, '-');

    // 8-4-4-4-12 format
    static constexpr int positions[] = {
        0,1,2,3,4,5,6,7, 9,10,11,12, 14,15,16,17,
        19,20,21,22, 24,25,26,27,28,29,30,31,32,33,34,35
    };

    for (int pos : positions) {
        uuid[pos] = hex[dist(gen)];
    }
    uuid[14] = '4'; // version 4
    uuid[19] = hex[dist2(gen)]; // variant 1

    return uuid;
}

/**
 * @brief Get current UTC time as ISO 8601 string.
 * @return Timestamp string.
 * @internal
 * @version 1.8.8
 */
std::string utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm utc{};
    gmtime_r(&time, &utc);

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc);
    return buf;
}

/**
 * @brief Create a ConversationRecord with generated fields.
 * @param title Title.
 * @param project_path Optional project path.
 * @param model_id Optional model ID.
 * @return Populated record.
 * @internal
 * @version 1.8.8
 */
ConversationRecord make_conversation(
        const std::string& title,
        const std::optional<std::string>& project_path,
        const std::optional<std::string>& model_id) {
    auto now = utc_timestamp();
    return {generate_uuid(), title, now, now, project_path, model_id, "{}"};
}

/**
 * @brief Create a DelegationRecord with generated fields.
 * @param parent_conversation_id Parent conversation.
 * @param child_conversation_id Child conversation.
 * @param delegating_tier Source tier.
 * @param target_tier Target tier.
 * @param task Task description.
 * @return Populated record.
 * @internal
 * @version 1.8.8
 */
DelegationRecord make_delegation(
        const std::string& parent_conversation_id,
        const std::string& child_conversation_id,
        const std::string& delegating_tier,
        const std::string& target_tier,
        const std::string& task) {
    return {generate_uuid(), parent_conversation_id,
            child_conversation_id, delegating_tier, target_tier,
            task, std::nullopt, "pending", std::nullopt,
            utc_timestamp(), std::nullopt};
}

} // namespace entropic
