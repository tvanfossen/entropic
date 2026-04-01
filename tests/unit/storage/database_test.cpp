/**
 * @file test_database.cpp
 * @brief BDD tests for SqliteDatabase and migration runner.
 * @version 1.8.8
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/storage/database.h>
#include <sqlite3.h>

#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

/**
 * @brief Create a temporary database path for testing.
 * @return Path to a temporary database file.
 * @utility
 * @version 1.8.8
 */
static fs::path temp_db_path() {
    auto dir = fs::temp_directory_path() / "entropic_test";
    fs::create_directories(dir);
    return dir / ("test_" + std::to_string(
        std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".db");
}

/**
 * @brief RAII cleanup for temp database files.
 * @internal
 * @version 1.8.8
 */
struct TempDb {
    fs::path path;
    entropic::SqliteDatabase db;

    /**
     * @brief Construct with auto-generated temp path.
     * @version 1.8.8
     */
    TempDb() : path(temp_db_path()), db(path) {}

    /**
     * @brief Destructor — remove temp file.
     * @version 1.8.8
     */
    ~TempDb() { fs::remove(path); }
};

SCENARIO("Database creates file and initializes", "[storage][database]") {
    GIVEN("A fresh database path") {
        TempDb t;

        WHEN("initialize is called") {
            bool ok = t.db.initialize();

            THEN("it succeeds and the file exists") {
                REQUIRE(ok);
                REQUIRE(fs::exists(t.path));
                REQUIRE(t.db.is_open());
            }
        }
    }
}

SCENARIO("Migrations run in order", "[storage][database]") {
    GIVEN("An initialized database") {
        TempDb t;
        REQUIRE(t.db.initialize());

        WHEN("we query the migrations table") {
            std::vector<std::string> names;
            t.db.fetch_all(
                "SELECT name FROM migrations ORDER BY id",
                nullptr,
                [&](sqlite3_stmt* s) {
                    auto* text = sqlite3_column_text(s, 0);
                    if (text) {
                        names.emplace_back(
                            reinterpret_cast<const char*>(text));
                    }
                });

            THEN("all 4 migrations are recorded") {
                REQUIRE(names.size() == 4);
                REQUIRE(names[0] == "001_initial");
                REQUIRE(names[1] == "002_fts");
                REQUIRE(names[2] == "003_delegations");
                REQUIRE(names[3] == "004_compaction_snapshots");
            }
        }
    }
}

SCENARIO("Migrations are idempotent", "[storage][database]") {
    GIVEN("An already-initialized database") {
        TempDb t;
        REQUIRE(t.db.initialize());

        WHEN("initialize is called again") {
            t.db.close();
            entropic::SqliteDatabase db2(t.path);
            bool ok = db2.initialize();

            THEN("it succeeds without re-applying migrations") {
                REQUIRE(ok);

                int count = 0;
                db2.fetch_one(
                    "SELECT COUNT(*) FROM migrations",
                    nullptr,
                    [&](sqlite3_stmt* s) {
                        count = sqlite3_column_int(s, 0);
                    });
                REQUIRE(count == 4);
            }
        }
    }
}

SCENARIO("Basic INSERT and SELECT round-trip", "[storage][database]") {
    GIVEN("An initialized database") {
        TempDb t;
        REQUIRE(t.db.initialize());

        WHEN("a conversation is inserted and fetched") {
            bool inserted = t.db.execute(
                "INSERT INTO conversations "
                "(id, title) VALUES (?, ?)",
                [](sqlite3_stmt* s) {
                    sqlite3_bind_text(s, 1, "test-id", -1, SQLITE_STATIC);
                    sqlite3_bind_text(s, 2, "Test Title", -1, SQLITE_STATIC);
                });

            std::string title;
            bool found = t.db.fetch_one(
                "SELECT title FROM conversations WHERE id = ?",
                [](sqlite3_stmt* s) {
                    sqlite3_bind_text(s, 1, "test-id", -1, SQLITE_STATIC);
                },
                [&](sqlite3_stmt* s) {
                    auto* p = sqlite3_column_text(s, 0);
                    if (p) title = reinterpret_cast<const char*>(p);
                });

            THEN("the data round-trips correctly") {
                REQUIRE(inserted);
                REQUIRE(found);
                REQUIRE(title == "Test Title");
            }
        }
    }
}

SCENARIO("FTS5 triggers populate on INSERT", "[storage][database]") {
    GIVEN("An initialized database with a conversation") {
        TempDb t;
        REQUIRE(t.db.initialize());

        t.db.execute(
            "INSERT INTO conversations (id, title) VALUES ('c1', 'Conv')",
            nullptr);

        WHEN("a message is inserted") {
            t.db.execute(
                "INSERT INTO messages "
                "(id, conversation_id, role, content) "
                "VALUES ('m1', 'c1', 'user', 'Hello world test')",
                nullptr);

            THEN("FTS5 index contains the content") {
                int count = 0;
                t.db.fetch_one(
                    "SELECT COUNT(*) FROM messages_fts "
                    "WHERE messages_fts MATCH 'hello'",
                    nullptr,
                    [&](sqlite3_stmt* s) {
                        count = sqlite3_column_int(s, 0);
                    });
                REQUIRE(count == 1);
            }
        }
    }
}

SCENARIO("FTS5 triggers clean up on DELETE", "[storage][database]") {
    GIVEN("A message in the FTS index") {
        TempDb t;
        REQUIRE(t.db.initialize());

        t.db.execute(
            "INSERT INTO conversations (id, title) VALUES ('c1', 'Conv')",
            nullptr);
        t.db.execute(
            "INSERT INTO messages "
            "(id, conversation_id, role, content) "
            "VALUES ('m1', 'c1', 'user', 'searchable content')",
            nullptr);

        WHEN("the message is deleted") {
            t.db.execute(
                "DELETE FROM messages WHERE id = 'm1'", nullptr);

            THEN("FTS5 index no longer matches") {
                int count = 0;
                t.db.fetch_one(
                    "SELECT COUNT(*) FROM messages_fts "
                    "WHERE messages_fts MATCH 'searchable'",
                    nullptr,
                    [&](sqlite3_stmt* s) {
                        count = sqlite3_column_int(s, 0);
                    });
                REQUIRE(count == 0);
            }
        }
    }
}

SCENARIO("CASCADE delete removes messages and delegations",
         "[storage][database]") {
    GIVEN("A conversation with messages and delegations") {
        TempDb t;
        REQUIRE(t.db.initialize());

        t.db.execute(
            "INSERT INTO conversations (id, title) VALUES ('p1', 'Parent')",
            nullptr);
        t.db.execute(
            "INSERT INTO conversations (id, title) VALUES ('c1', 'Child')",
            nullptr);
        t.db.execute(
            "INSERT INTO messages "
            "(id, conversation_id, role, content) "
            "VALUES ('m1', 'p1', 'user', 'hello')",
            nullptr);
        t.db.execute(
            "INSERT INTO delegations "
            "(id, parent_conversation_id, child_conversation_id, "
            "delegating_tier, target_tier, task) "
            "VALUES ('d1', 'p1', 'c1', 'lead', 'eng', 'do something')",
            nullptr);

        WHEN("the parent conversation is deleted") {
            t.db.execute(
                "DELETE FROM conversations WHERE id = 'p1'", nullptr);

            THEN("messages and delegations are also deleted") {
                int msg_count = 0;
                t.db.fetch_one(
                    "SELECT COUNT(*) FROM messages WHERE conversation_id = 'p1'",
                    nullptr,
                    [&](sqlite3_stmt* s) {
                        msg_count = sqlite3_column_int(s, 0);
                    });
                REQUIRE(msg_count == 0);

                int del_count = 0;
                t.db.fetch_one(
                    "SELECT COUNT(*) FROM delegations "
                    "WHERE parent_conversation_id = 'p1'",
                    nullptr,
                    [&](sqlite3_stmt* s) {
                        del_count = sqlite3_column_int(s, 0);
                    });
                REQUIRE(del_count == 0);
            }
        }
    }
}

SCENARIO("Concurrent access does not corrupt", "[storage][database]") {
    GIVEN("An initialized database") {
        TempDb t;
        REQUIRE(t.db.initialize());

        t.db.execute(
            "INSERT INTO conversations (id, title) VALUES ('c1', 'Conv')",
            nullptr);

        WHEN("multiple threads insert messages concurrently") {
            constexpr int thread_count = 4;
            constexpr int msgs_per_thread = 10;
            std::vector<std::thread> threads;

            for (int i = 0; i < thread_count; ++i) {
                threads.emplace_back([&, i]() {
                    for (int j = 0; j < msgs_per_thread; ++j) {
                        auto id = "m_" + std::to_string(i) +
                                  "_" + std::to_string(j);
                        t.db.execute(
                            "INSERT INTO messages "
                            "(id, conversation_id, role, content) "
                            "VALUES (?, 'c1', 'user', 'msg')",
                            [&](sqlite3_stmt* s) {
                                sqlite3_bind_text(s, 1, id.c_str(),
                                                  -1, SQLITE_TRANSIENT);
                            });
                    }
                });
            }

            for (auto& th : threads) th.join();

            THEN("all messages are present") {
                int count = 0;
                t.db.fetch_one(
                    "SELECT COUNT(*) FROM messages",
                    nullptr,
                    [&](sqlite3_stmt* s) {
                        count = sqlite3_column_int(s, 0);
                    });
                REQUIRE(count == thread_count * msgs_per_thread);
            }
        }
    }
}
