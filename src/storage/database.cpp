/**
 * @file database.cpp
 * @brief SqliteDatabase implementation with migration runner.
 * @version 1.8.8
 */

#include <entropic/storage/database.h>

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <array>
#include <cstring>

namespace entropic {

// ── Migration definitions ─────────────────────────────────

/**
 * @brief A single named migration.
 * @internal
 * @version 1.8.8
 */
struct Migration {
    const char* name; ///< Migration name (e.g., "001_initial")
    const char* sql;  ///< SQL to execute
};

/// @internal
static constexpr const char* MIGRATION_001_INITIAL = R"sql(
CREATE TABLE IF NOT EXISTS conversations (
    id TEXT PRIMARY KEY,
    title TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    project_path TEXT,
    model_id TEXT,
    metadata TEXT DEFAULT '{}'
);

CREATE TABLE IF NOT EXISTS messages (
    id TEXT PRIMARY KEY,
    conversation_id TEXT NOT NULL,
    role TEXT NOT NULL CHECK(role IN ('user', 'assistant', 'system', 'tool')),
    content TEXT NOT NULL,
    tool_calls TEXT DEFAULT '[]',
    tool_results TEXT DEFAULT '[]',
    token_count INTEGER DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    is_compacted BOOLEAN DEFAULT FALSE,
    identity_tier TEXT,
    FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS tool_executions (
    id TEXT PRIMARY KEY,
    message_id TEXT,
    server_name TEXT NOT NULL,
    tool_name TEXT NOT NULL,
    arguments TEXT DEFAULT '{}',
    result TEXT,
    duration_ms INTEGER DEFAULT 0,
    status TEXT CHECK(status IN ('success', 'error', 'timeout')),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (message_id) REFERENCES messages(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_messages_conversation
    ON messages(conversation_id);
CREATE INDEX IF NOT EXISTS idx_messages_created
    ON messages(created_at);
CREATE INDEX IF NOT EXISTS idx_conversations_updated
    ON conversations(updated_at);
)sql";

/// @internal
static constexpr const char* MIGRATION_002_FTS = R"sql(
CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
    content,
    content='messages',
    content_rowid='rowid'
);

CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN
    INSERT INTO messages_fts(rowid, content) VALUES (NEW.rowid, NEW.content);
END;

CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN
    INSERT INTO messages_fts(messages_fts, rowid, content)
        VALUES('delete', OLD.rowid, OLD.content);
END;

CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE ON messages BEGIN
    INSERT INTO messages_fts(messages_fts, rowid, content)
        VALUES('delete', OLD.rowid, OLD.content);
    INSERT INTO messages_fts(rowid, content) VALUES (NEW.rowid, NEW.content);
END;
)sql";

/// @internal
static constexpr const char* MIGRATION_003_DELEGATIONS = R"sql(
CREATE TABLE IF NOT EXISTS delegations (
    id TEXT PRIMARY KEY,
    parent_conversation_id TEXT NOT NULL,
    child_conversation_id TEXT NOT NULL,
    delegating_tier TEXT NOT NULL,
    target_tier TEXT NOT NULL,
    task TEXT NOT NULL,
    max_turns INTEGER,
    status TEXT NOT NULL DEFAULT 'pending'
        CHECK(status IN ('pending', 'running', 'completed', 'failed')),
    result_summary TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    completed_at TIMESTAMP,
    FOREIGN KEY (parent_conversation_id)
        REFERENCES conversations(id) ON DELETE CASCADE,
    FOREIGN KEY (child_conversation_id)
        REFERENCES conversations(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_delegations_parent
    ON delegations(parent_conversation_id);
CREATE INDEX IF NOT EXISTS idx_delegations_child
    ON delegations(child_conversation_id);
)sql";

/// @internal
static constexpr const char* MIGRATION_004_COMPACTION_SNAPSHOTS = R"sql(
CREATE TABLE IF NOT EXISTS compaction_snapshots (
    id TEXT PRIMARY KEY,
    conversation_id TEXT NOT NULL,
    messages_json TEXT NOT NULL,
    message_count INTEGER NOT NULL,
    token_count_estimate INTEGER,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (conversation_id)
        REFERENCES conversations(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_snapshots_conversation
    ON compaction_snapshots(conversation_id);
)sql";

/// @internal
static constexpr std::array<Migration, 4> MIGRATIONS = {{
    {"001_initial",             MIGRATION_001_INITIAL},
    {"002_fts",                 MIGRATION_002_FTS},
    {"003_delegations",         MIGRATION_003_DELEGATIONS},
    {"004_compaction_snapshots", MIGRATION_004_COMPACTION_SNAPSHOTS},
}};

// ── SqliteDatabase implementation ─────────────────────────

/**
 * @brief Construct with database file path.
 * @param db_path Path to SQLite file.
 * @internal
 * @version 1.8.8
 */
SqliteDatabase::SqliteDatabase(const std::filesystem::path& db_path)
    : db_path_(db_path) {}

/**
 * @brief Destructor — closes connection if open.
 * @internal
 * @version 1.8.8
 */
SqliteDatabase::~SqliteDatabase() {
    close();
}

/**
 * @brief Initialize database and run pending migrations.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteDatabase::initialize() {
    std::lock_guard lock(mutex_);

    // Ensure parent directory exists
    auto parent = db_path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    int rc = sqlite3_open_v2(
        db_path_.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);

    if (rc != SQLITE_OK) {
        spdlog::error("Failed to open database {}: {}",
                      db_path_.string(), sqlite3_errmsg(db_));
        return false;
    }

    // Enable foreign keys and WAL mode
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA journal_mode = WAL", nullptr, nullptr, nullptr);

    if (!run_migrations()) {
        spdlog::error("Migration failed for {}", db_path_.string());
        return false;
    }

    spdlog::info("Database initialized: {}", db_path_.string());
    return true;
}

/**
 * @brief Close database connection.
 * @internal
 * @version 1.8.8
 */
void SqliteDatabase::close() {
    std::lock_guard lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

/**
 * @brief Check if database is open.
 * @return true if connection is active.
 * @internal
 * @version 1.8.8
 */
bool SqliteDatabase::is_open() const {
    std::lock_guard lock(mutex_);
    return db_ != nullptr;
}

/**
 * @brief Prepare a SQL statement.
 * @param sql SQL text.
 * @return Prepared statement or nullptr on error.
 * @internal
 * @version 1.8.8
 */
sqlite3_stmt* SqliteDatabase::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        db_, sql.data(), static_cast<int>(sql.size()),
        &stmt, nullptr);

    if (rc != SQLITE_OK) {
        spdlog::error("SQL prepare failed: {} — {}",
                      sqlite3_errmsg(db_), std::string(sql));
        return nullptr;
    }
    return stmt;
}

/**
 * @brief Execute a write statement with optional parameter binding.
 * @param sql SQL statement with ? placeholders.
 * @param binder Function to bind parameters.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteDatabase::execute(std::string_view sql,
                             std::function<void(sqlite3_stmt*)> binder) {
    std::lock_guard lock(mutex_);
    if (!db_) return false;

    auto* stmt = prepare(sql);
    if (!stmt) return false;

    if (binder) binder(stmt);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    bool ok = (rc == SQLITE_DONE);
    if (!ok) {
        spdlog::error("SQL execute failed: {}", sqlite3_errmsg(db_));
    }
    return ok;
}

/**
 * @brief Execute raw SQL (multiple statements, no binding).
 * @param sql SQL text.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteDatabase::execute_raw(std::string_view sql) {
    std::lock_guard lock(mutex_);
    if (!db_) return false;

    char* err_msg = nullptr;
    int rc = sqlite3_exec(
        db_, std::string(sql).c_str(),
        nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        spdlog::error("SQL exec failed: {}",
                      err_msg ? err_msg : "unknown error");
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

/**
 * @brief Fetch a single row.
 * @param sql SQL SELECT statement.
 * @param binder Function to bind parameters.
 * @param extractor Function to extract columns from result row.
 * @return true if row found.
 * @internal
 * @version 1.8.8
 */
bool SqliteDatabase::fetch_one(
        std::string_view sql,
        std::function<void(sqlite3_stmt*)> binder,
        std::function<void(sqlite3_stmt*)> extractor) {
    std::lock_guard lock(mutex_);
    if (!db_) return false;

    auto* stmt = prepare(sql);
    if (!stmt) return false;

    if (binder) binder(stmt);

    int rc = sqlite3_step(stmt);
    bool found = (rc == SQLITE_ROW);
    if (found && extractor) {
        extractor(stmt);
    }

    sqlite3_finalize(stmt);
    return found;
}

/**
 * @brief Fetch all matching rows.
 * @param sql SQL SELECT statement.
 * @param binder Function to bind parameters.
 * @param row_handler Called for each result row.
 * @return Number of rows fetched.
 * @internal
 * @version 1.8.8
 */
size_t SqliteDatabase::fetch_all(
        std::string_view sql,
        std::function<void(sqlite3_stmt*)> binder,
        std::function<void(sqlite3_stmt*)> row_handler) {
    std::lock_guard lock(mutex_);
    if (!db_) return 0;

    auto* stmt = prepare(sql);
    if (!stmt) return 0;

    if (binder) binder(stmt);

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (row_handler) row_handler(stmt);
        ++count;
    }

    sqlite3_finalize(stmt);
    return count;
}

/**
 * @brief Get the underlying sqlite3 handle.
 * @return Raw sqlite3 pointer.
 * @internal
 * @version 1.8.8
 */
sqlite3* SqliteDatabase::raw_handle() const {
    return db_;
}

/**
 * @brief Get names of already-applied migrations.
 * @param db SQLite database handle.
 * @return Vector of applied migration names.
 * @internal
 * @version 1.8.8
 */
static std::vector<std::string> get_applied_migrations(sqlite3* db) {
    std::vector<std::string> applied;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT name FROM migrations", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return applied;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* text = sqlite3_column_text(stmt, 0);
        if (text) {
            applied.emplace_back(reinterpret_cast<const char*>(text));
        }
    }
    sqlite3_finalize(stmt);
    return applied;
}

/**
 * @brief Apply a single migration and record it.
 * @param db SQLite database handle.
 * @param mig Migration to apply.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
static bool apply_migration(sqlite3* db, const Migration& mig) {
    spdlog::info("Running migration: {}", mig.name);

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, mig.sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        spdlog::error("Migration {} failed: {}",
                      mig.name, err_msg ? err_msg : "unknown");
        sqlite3_free(err_msg);
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO migrations (name) VALUES (?)",
        -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, mig.name, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    return true;
}

/**
 * @brief Check if a migration name is in the applied list.
 * @param applied Applied migration names.
 * @param name Migration name to check.
 * @return true if already applied.
 * @internal
 * @version 1.8.8
 */
static bool is_applied(const std::vector<std::string>& applied,
                       const char* name) {
    for (const auto& n : applied) {
        if (n == name) return true;
    }
    return false;
}

/**
 * @brief Run all pending migrations sequentially.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SqliteDatabase::run_migrations() {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS migrations ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT UNIQUE,"
        "  applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ")",
        nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        spdlog::error("Failed to create migrations table: {}",
                      err_msg ? err_msg : "unknown");
        sqlite3_free(err_msg);
        return false;
    }

    auto applied = get_applied_migrations(db_);
    for (const auto& mig : MIGRATIONS) {
        if (is_applied(applied, mig.name)) continue;
        if (!apply_migration(db_, mig)) return false;
    }
    return true;
}

} // namespace entropic
