/**
 * @file database.h
 * @brief Thread-safe SQLite database wrapper with migration support.
 *
 * Internal to librentropic-storage.so. Wraps sqlite3 with mutex
 * serialization and a sequential migration runner.
 *
 * @version 1.8.8
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace entropic {

/**
 * @brief Thread-safe SQLite database wrapper.
 *
 * Opens a SQLite database with SQLITE_OPEN_FULLMUTEX as a safety net.
 * Application-level std::mutex serializes all operations to prevent
 * concurrent access patterns.
 *
 * @par Lifecycle:
 * @code
 *   SqliteDatabase db("/path/to/entropic.db");
 *   db.initialize();  // Creates file, runs migrations
 *   db.execute("INSERT INTO ...", [](auto* s) { ... });
 *   db.close();       // Or let destructor handle it
 * @endcode
 *
 * @version 1.8.8
 */
class SqliteDatabase {
public:
    /**
     * @brief Construct with database file path.
     * @param db_path Path to SQLite file. Created if absent.
     * @version 1.8.8
     */
    explicit SqliteDatabase(const std::filesystem::path& db_path);

    /**
     * @brief Destructor — closes connection if open.
     * @version 1.8.8
     */
    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    /**
     * @brief Initialize database and run pending migrations.
     * @return true on success, false on error.
     * @version 1.8.8
     */
    bool initialize();

    /**
     * @brief Close database connection.
     * @version 1.8.8
     */
    void close();

    /**
     * @brief Check if database is open.
     * @return true if connection is active.
     * @version 1.8.8
     */
    bool is_open() const;

    /**
     * @brief Execute a write statement (INSERT, UPDATE, DELETE).
     * @param sql SQL statement with ? placeholders.
     * @param binder Function to bind parameters to the prepared statement.
     * @return true on success.
     * @version 1.8.8
     */
    bool execute(std::string_view sql,
                 std::function<void(sqlite3_stmt*)> binder = nullptr);

    /**
     * @brief Execute raw SQL (multiple statements, no binding).
     * @param sql SQL text (may contain multiple semicolon-separated statements).
     * @return true on success.
     * @version 1.8.8
     */
    bool execute_raw(std::string_view sql);

    /**
     * @brief Fetch a single row.
     * @param sql SQL SELECT statement.
     * @param binder Function to bind parameters.
     * @param extractor Function to extract columns from result row.
     * @return true if row found.
     * @version 1.8.8
     */
    bool fetch_one(std::string_view sql,
                   std::function<void(sqlite3_stmt*)> binder,
                   std::function<void(sqlite3_stmt*)> extractor);

    /**
     * @brief Fetch all matching rows.
     * @param sql SQL SELECT statement.
     * @param binder Function to bind parameters.
     * @param row_handler Called for each result row.
     * @return Number of rows fetched.
     * @version 1.8.8
     */
    size_t fetch_all(std::string_view sql,
                     std::function<void(sqlite3_stmt*)> binder,
                     std::function<void(sqlite3_stmt*)> row_handler);

    /**
     * @brief Get the underlying sqlite3 handle (for advanced use).
     * @return Raw sqlite3 pointer (may be nullptr if closed).
     * @version 1.8.8
     */
    sqlite3* raw_handle() const;

private:
    /**
     * @brief Run all pending migrations.
     * @return true on success.
     * @version 1.8.8
     */
    bool run_migrations();

    /**
     * @brief Prepare a SQL statement.
     * @param sql SQL text.
     * @return Prepared statement or nullptr on error.
     * @version 1.8.8
     */
    sqlite3_stmt* prepare(std::string_view sql);

    std::filesystem::path db_path_; ///< Database file path
    sqlite3* db_ = nullptr;        ///< SQLite connection handle
    mutable std::mutex mutex_;      ///< Serializes all database access
};

} // namespace entropic
