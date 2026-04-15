// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_backend.cpp
 * @brief BDD tests for SqliteStorageBackend CRUD operations.
 * @version 1.8.8
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/storage/backend.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

/**
 * @brief RAII temp storage for tests.
 * @internal
 * @version 1.8.8
 */
struct TempStorage {
    fs::path path;
    entropic::SqliteStorageBackend storage;

    /**
     * @brief Construct, initialize storage.
     * @version 1.8.8
     */
    TempStorage()
        : path(fs::temp_directory_path() / "entropic_test" /
               ("backend_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db")),
          storage(path) {
        storage.initialize();
    }

    /**
     * @brief Destructor — cleanup.
     * @version 1.8.8
     */
    ~TempStorage() { fs::remove(path); }
};

SCENARIO("Create conversation returns valid UUID",
         "[storage][backend]") {
    GIVEN("An initialized storage backend") {
        TempStorage t;

        WHEN("a conversation is created") {
            auto id = t.storage.create_conversation("Test");

            THEN("it returns a non-empty UUID string") {
                REQUIRE(!id.empty());
                REQUIRE(id.size() == 36); // UUID format
                REQUIRE(id[8] == '-');
                REQUIRE(id[14] == '4'); // v4
            }
        }
    }
}

SCENARIO("Save and load conversation round-trips",
         "[storage][backend]") {
    GIVEN("A conversation with saved messages") {
        TempStorage t;
        auto conv_id = t.storage.create_conversation("Round-trip test");

        json messages = json::array({
            {{"role", "user"}, {"content", "Hello"}},
            {{"role", "assistant"}, {"content", "Hi there"}},
        });
        t.storage.save_messages(conv_id, messages.dump());

        WHEN("the conversation is loaded") {
            std::string result;
            bool found = t.storage.load_conversation(conv_id, result);

            THEN("all messages are present and correct") {
                REQUIRE(found);
                auto j = json::parse(result);
                REQUIRE(j["conversation"]["id"] == conv_id);
                REQUIRE(j["messages"].size() == 2);
                REQUIRE(j["messages"][0]["role"] == "user");
                REQUIRE(j["messages"][0]["content"] == "Hello");
                REQUIRE(j["messages"][1]["role"] == "assistant");
            }
        }
    }
}

SCENARIO("List conversations with pagination",
         "[storage][backend]") {
    GIVEN("Multiple conversations") {
        TempStorage t;
        t.storage.create_conversation("Conv 1");
        t.storage.create_conversation("Conv 2");
        t.storage.create_conversation("Conv 3");

        WHEN("listed with limit 2, offset 0") {
            std::string result;
            t.storage.list_conversations(2, 0, result);
            auto arr = json::parse(result);

            THEN("returns 2 results") {
                REQUIRE(arr.size() == 2);
            }
        }

        WHEN("listed with limit 2, offset 2") {
            std::string result;
            t.storage.list_conversations(2, 2, result);
            auto arr = json::parse(result);

            THEN("returns 1 result") {
                REQUIRE(arr.size() == 1);
            }
        }
    }
}

SCENARIO("Delete conversation cascades",
         "[storage][backend]") {
    GIVEN("A conversation with messages") {
        TempStorage t;
        auto conv_id = t.storage.create_conversation("To delete");
        json msgs = json::array({{{"role", "user"}, {"content", "bye"}}});
        t.storage.save_messages(conv_id, msgs.dump());

        WHEN("the conversation is deleted") {
            bool ok = t.storage.delete_conversation(conv_id);

            THEN("loading returns not found") {
                REQUIRE(ok);
                std::string result;
                REQUIRE_FALSE(t.storage.load_conversation(conv_id, result));
            }
        }
    }
}

SCENARIO("Update conversation title",
         "[storage][backend]") {
    GIVEN("An existing conversation") {
        TempStorage t;
        auto conv_id = t.storage.create_conversation("Old Title");

        WHEN("the title is updated") {
            t.storage.update_title(conv_id, "New Title");

            THEN("loading shows the new title") {
                std::string result;
                t.storage.load_conversation(conv_id, result);
                auto j = json::parse(result);
                REQUIRE(j["conversation"]["title"] == "New Title");
            }
        }
    }
}

SCENARIO("FTS5 search returns results with snippets",
         "[storage][backend]") {
    GIVEN("A conversation with searchable messages") {
        TempStorage t;
        auto conv_id = t.storage.create_conversation("Searchable");
        json msgs = json::array({
            {{"role", "user"}, {"content", "The quantum entanglement experiment"}},
            {{"role", "assistant"}, {"content", "Let me help with quantum physics"}},
        });
        t.storage.save_messages(conv_id, msgs.dump());

        WHEN("searching for 'quantum'") {
            std::string result;
            t.storage.search_conversations("quantum", 10, result);
            auto arr = json::parse(result);

            THEN("results include the conversation") {
                REQUIRE(arr.size() >= 1);
                REQUIRE(arr[0]["id"] == conv_id);
                REQUIRE(!arr[0]["snippet"].get<std::string>().empty());
            }
        }
    }
}

SCENARIO("Delegation lifecycle", "[storage][backend]") {
    GIVEN("A parent conversation") {
        TempStorage t;
        auto parent_id = t.storage.create_conversation("Parent");

        WHEN("a delegation is created") {
            std::string del_id, child_id;
            bool ok = t.storage.create_delegation(
                parent_id, "lead", "eng", "Implement feature", 10,
                del_id, child_id);

            THEN("both IDs are returned") {
                REQUIRE(ok);
                REQUIRE(del_id.size() == 36);
                REQUIRE(child_id.size() == 36);
            }

            AND_WHEN("the delegation is completed") {
                t.storage.complete_delegation(
                    del_id, "completed", "Done successfully");

                THEN("get_delegations shows completed status") {
                    std::string result;
                    t.storage.get_delegations(parent_id, result);
                    auto arr = json::parse(result);
                    REQUIRE(arr.size() == 1);
                    REQUIRE(arr[0]["status"] == "completed");
                    REQUIRE(arr[0]["result_summary"] == "Done successfully");
                }
            }

            AND_WHEN("the delegation fails") {
                t.storage.complete_delegation(
                    del_id, "failed", "Error occurred");

                THEN("get_delegations shows failed status") {
                    std::string result;
                    t.storage.get_delegations(parent_id, result);
                    auto arr = json::parse(result);
                    REQUIRE(arr[0]["status"] == "failed");
                }
            }
        }
    }
}

SCENARIO("Compaction snapshot saved", "[storage][backend]") {
    GIVEN("A conversation") {
        TempStorage t;
        auto conv_id = t.storage.create_conversation("Compaction test");

        WHEN("a snapshot is saved") {
            json msgs = json::array({
                {{"role", "user"}, {"content", "Message 1"}},
                {{"role", "assistant"}, {"content", "Response 1"}},
            });
            bool ok = t.storage.save_snapshot(conv_id, msgs.dump());

            THEN("it succeeds") {
                REQUIRE(ok);
            }
        }
    }
}

SCENARIO("Storage statistics", "[storage][backend]") {
    GIVEN("A storage with conversations and messages") {
        TempStorage t;
        auto id1 = t.storage.create_conversation("Conv 1");
        auto id2 = t.storage.create_conversation("Conv 2");
        json msgs = json::array({
            {{"role", "user"}, {"content", "msg"}, {"token_count", 10}},
        });
        t.storage.save_messages(id1, msgs.dump());
        t.storage.save_messages(id2, msgs.dump());

        WHEN("stats are queried") {
            std::string result;
            t.storage.get_stats(result);
            auto stats = json::parse(result);

            THEN("counts are correct") {
                REQUIRE(stats["total_conversations"] == 2);
                REQUIRE(stats["total_messages"] == 2);
                REQUIRE(stats["total_tokens"] == 20);
            }
        }
    }
}
