// SPDX-License-Identifier: Apache-2.0
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

/**
 * @brief Bind optional int to a parameter position (NULL if unset).
 * @param stmt Prepared statement.
 * @param idx Parameter index (1-based).
 * @param val Value to bind.
 * @utility
 * @version 2.3.7
 */
static void bind_opt_int(sqlite3_stmt* stmt, int idx,
                         const std::optional<int>& val) {
    if (val) {
        sqlite3_bind_int(stmt, idx, *val);
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

namespace {

/// One message row staged for INSERT INTO messages.
struct MessageRow {
    std::string id;
    std::string role;
    std::string content;
    std::string tool_calls;
    std::string tool_results;
    long long token_count;
    bool is_compacted;
    std::optional<std::string> tier;
};

/**
 * @brief Build a MessageRow from one message JSON object.
 * @param m Message JSON.
 * @return Populated row (new uuid, defaulted fields).
 * @utility
 * @version 2.3.7
 */
MessageRow build_message_row(const json& m) {
    MessageRow r;
    r.id = generate_uuid();
    r.role = m.value("role", "");
    r.content = m.value("content", "");
    r.tool_calls = m.contains("tool_calls") ? m["tool_calls"].dump() : "[]";
    r.tool_results =
        m.contains("tool_results") ? m["tool_results"].dump() : "[]";
    r.token_count = m.value("token_count", 0);
    r.is_compacted = m.value("is_compacted", false);
    if (m.contains("identity_tier") && !m["identity_tier"].is_null()) {
        r.tier = m["identity_tier"].get<std::string>();
    }
    return r;
}

/**
 * @brief Bind a MessageRow to the messages INSERT statement.
 * @param s Prepared statement.
 * @param conversation_id Owning conversation.
 * @param now created_at timestamp.
 * @param r Row to bind.
 * @utility
 * @version 2.3.7
 */
void bind_message_insert(sqlite3_stmt* s, const std::string& conversation_id,
                         const std::string& now, const MessageRow& r) {
    sqlite3_bind_text(s, 1, r.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, conversation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, r.role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, r.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, r.tool_calls.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 6, r.tool_results.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 7, r.token_count);
    sqlite3_bind_text(s, 8, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 9, r.is_compacted ? 1 : 0);
    bind_opt_text(s, 10, r.tier);
}

}  // namespace

/**
 * @brief Save messages to a conversation.
 * @param conversation_id Conversation ID.
 * @param messages_json JSON array of message objects.
 * @return true on success.
 * @internal
 * @version 2.3.7
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

    static constexpr const char* kInsertSql =
        "INSERT INTO messages "
        "(id, conversation_id, role, content, tool_calls, tool_results, "
        "token_count, created_at, is_compacted, identity_tier) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    for (const auto& m : msgs) {
        auto row = build_message_row(m);
        db_.execute(kInsertSql, [&](sqlite3_stmt* s) {
            bind_message_insert(s, conversation_id, now, row);
        });
    }

    return true;
}

namespace {

/**
 * @brief Map a conversations row to its JSON object.
 * @param s Stepped statement (SELECT * FROM conversations).
 * @return JSON conversation object.
 * @utility
 * @version 2.3.7
 */
json conversation_row_to_json(sqlite3_stmt* s) {
    json o;
    o["id"] = col_text(s, 0);
    o["title"] = col_text(s, 1);
    o["created_at"] = col_text(s, 2);
    o["updated_at"] = col_text(s, 3);
    o["project_path"] = col_opt_text(s, 4).value_or("");
    o["model_id"] = col_opt_text(s, 5).value_or("");
    o["metadata"] = json::parse(col_text(s, 6), nullptr, false);
    return o;
}

/**
 * @brief Map a messages row to its JSON object.
 * @param s Stepped statement (SELECT * FROM messages).
 * @return JSON message object.
 * @utility
 * @version 2.3.7
 */
json message_row_to_json(sqlite3_stmt* s) {
    json m;
    m["id"] = col_text(s, 0);
    m["role"] = col_text(s, 2);
    m["content"] = col_text(s, 3);
    m["tool_calls"] = json::parse(col_text(s, 4), nullptr, false);
    m["tool_results"] = json::parse(col_text(s, 5), nullptr, false);
    m["token_count"] = sqlite3_column_int64(s, 6);
    m["created_at"] = col_text(s, 7);
    m["is_compacted"] = sqlite3_column_int(s, 8) != 0;
    m["identity_tier"] = col_opt_text(s, 9).value_or("");
    return m;
}

}  // namespace

/**
 * @brief Load a conversation with its messages.
 * @param conversation_id Conversation ID.
 * @param[out] result_json JSON result.
 * @return true if found.
 * @internal
 * @version 2.3.7
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
        [&](sqlite3_stmt* s) { conv_obj = conversation_row_to_json(s); });

    if (!found) return false;

    json messages = json::array();
    db_.fetch_all(
        "SELECT * FROM messages WHERE conversation_id = ? "
        "ORDER BY created_at ASC",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, conversation_id.c_str(), -1, SQLITE_TRANSIENT);
        },
        [&](sqlite3_stmt* s) {
            messages.push_back(message_row_to_json(s));
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
 * @brief Bind a Delegation record onto the delegations-INSERT
 *        statement's 11 placeholders.
 *
 * Extracted from `create_delegation` (v2.1.12, gh#48) so the public
 * function fits the knots SLOC ≤ 50 threshold after the defense-in-
 * depth additions. Bind order matches the INSERT column order.
 *
 * @param s Prepared statement.
 * @param rec Delegation record (read-only).
 * @internal
 * @version 2.3.7
 */
static void bind_delegation_insert(sqlite3_stmt* s,
                                   const DelegationRecord& rec) {
    sqlite3_bind_text(s, 1, rec.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, rec.parent_conversation_id.c_str(),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, rec.child_conversation_id.c_str(),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, rec.delegating_tier.c_str(),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, rec.target_tier.c_str(),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 6, rec.task.c_str(), -1, SQLITE_TRANSIENT);
    bind_opt_int(s, 7, rec.max_turns);
    sqlite3_bind_text(s, 8, rec.status.c_str(), -1, SQLITE_TRANSIENT);
    bind_opt_text(s, 9, rec.result_summary);
    sqlite3_bind_text(s, 10, rec.created_at.c_str(),
                      -1, SQLITE_TRANSIENT);
    bind_opt_text(s, 11, rec.completed_at);
}

/**
 * @brief Reject an empty parent_conversation_id with a clear error
 *        and reset the out params (gh#48 defense-in-depth).
 *
 * Pre-v2.1.12 an empty parent slipped through to the FK-bound INSERT
 * and failed silently against `conversations(id)`. The engine
 * populates the root conversation at run() init; this guards
 * against any caller bypassing that path.
 *
 * @param parent_conversation_id Parent ID (must be non-empty).
 * @param delegating_tier Diagnostic context.
 * @param target_tier Diagnostic context.
 * @param[out] delegation_id Cleared on rejection.
 * @param[out] child_conversation_id Cleared on rejection.
 * @return true if the parent is valid; false (and out params cleared)
 *         if empty.
 * @internal
 * @version 2.1.12
 */
static bool guard_parent_conversation(
    const std::string& parent_conversation_id,
    const std::string& delegating_tier,
    const std::string& target_tier,
    std::string& delegation_id,
    std::string& child_conversation_id) {
    if (!parent_conversation_id.empty()) { return true; }
    logger->error("create_delegation refused: parent_conversation_id "
                  "is empty (delegating_tier={}, target_tier={}). "
                  "Root conversation must be created before "
                  "delegating (gh#48).",
                  delegating_tier, target_tier);
    delegation_id.clear();
    child_conversation_id.clear();
    return false;
}

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
 * @version 2.1.12
 */
bool SqliteStorageBackend::create_delegation(
        const std::string& parent_conversation_id,
        const std::string& delegating_tier,
        const std::string& target_tier,
        const std::string& task,
        int max_turns,
        std::string& delegation_id,
        std::string& child_conversation_id) {
    if (!guard_parent_conversation(parent_conversation_id,
                                   delegating_tier, target_tier,
                                   delegation_id,
                                   child_conversation_id)) {
        return false;
    }

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
        [&](sqlite3_stmt* s) { bind_delegation_insert(s, rec); });

    delegation_id = rec.id;
    // gh#48 defense-in-depth (v2.1.12): log success only when the
    // INSERT actually succeeded. Pre-v2.1.12 this fired
    // unconditionally and masked the FK failure logged one line up
    // by `database.cpp`'s `execute()` from anyone scanning for
    // "Created delegation" in the session log.
    if (ok) {
        logger->info("Created delegation {}: {} -> {}",
                     rec.id, delegating_tier, target_tier);
    } else {
        logger->error("Failed to insert delegation {} ({} -> {}): "
                      "parent_conversation_id='{}' — check the SQL "
                      "execute error logged immediately above",
                      rec.id, delegating_tier, target_tier,
                      parent_conversation_id);
    }
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

namespace {

/**
 * @brief Map a full delegations row to its JSON object.
 * @param s Stepped statement (SELECT * FROM delegations).
 * @return JSON delegation object (all columns).
 * @utility
 * @version 2.3.7
 */
json delegation_row_to_json(sqlite3_stmt* s) {
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
    return entry;
}

/**
 * @brief Map a delegations row to a search-result summary object.
 *
 * Subset of delegation_row_to_json: omits max_turns + created_at, which
 * search results historically don't carry.
 *
 * @param s Stepped statement (SELECT * FROM delegations).
 * @return JSON summary object.
 * @utility
 * @version 2.3.7
 */
json delegation_summary_to_json(sqlite3_stmt* s) {
    json entry;
    entry["id"] = col_text(s, 0);
    entry["parent_conversation_id"] = col_text(s, 1);
    entry["child_conversation_id"] = col_text(s, 2);
    entry["delegating_tier"] = col_text(s, 3);
    entry["target_tier"] = col_text(s, 4);
    entry["task"] = col_text(s, 5);
    entry["status"] = col_text(s, 7);
    entry["result_summary"] = col_opt_text(s, 8).value_or("");
    entry["completed_at"] = col_opt_text(s, 10).value_or("");
    return entry;
}

}  // namespace

/**
 * @brief Get delegations for a parent conversation.
 * @param conversation_id Parent conversation ID.
 * @param[out] result_json JSON array of delegation records.
 * @return true on success.
 * @internal
 * @version 2.3.7
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
            arr.push_back(delegation_row_to_json(s));
        });

    result_json = arr.dump();
    return true;
}

/**
 * @brief Look up a single delegation record by id (gh#32, v2.1.6).
 * @param delegation_id Delegation id.
 * @param[out] result_json Object JSON of the delegation row.
 * @return true if found and parsed.
 * @internal
 * @version 2.3.7
 */
bool SqliteStorageBackend::get_delegation_by_id(
        const std::string& delegation_id,
        std::string& result_json) {
    bool found = false;
    json entry;
    db_.fetch_all(
        "SELECT * FROM delegations WHERE id = ? LIMIT 1",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, delegation_id.c_str(), -1,
                              SQLITE_TRANSIENT);
        },
        [&](sqlite3_stmt* s) {
            found = true;
            entry = delegation_row_to_json(s);
        });
    if (!found) {
        return false;
    }
    result_json = entry.dump();
    return true;
}

/**
 * @brief Substring-match delegations across all conversations.
 *
 * Bound to top-N most recently completed records. Uses sqlite's
 * `LIKE` for portability — FTS5 is wired for messages, not
 * delegation summaries.
 *
 * @param query        Substring to match (LIKE %query%).
 * @param max_results  Cap on returned rows.
 * @param[out] result_json JSON array of delegation rows.
 * @return true on success.
 * @internal
 * @version 2.3.7
 */
bool SqliteStorageBackend::search_delegations(
        const std::string& query, int max_results,
        std::string& result_json) {
    json arr = json::array();
    std::string like = "%" + query + "%";
    db_.fetch_all(
        "SELECT * FROM delegations "
        "WHERE result_summary LIKE ? AND status = 'completed' "
        "ORDER BY completed_at DESC LIMIT ?",
        [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, like.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 2, max_results);
        },
        [&](sqlite3_stmt* s) {
            arr.push_back(delegation_summary_to_json(s));
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
