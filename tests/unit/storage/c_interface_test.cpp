/**
 * @file test_c_interface.cpp
 * @brief BDD tests for the storage C boundary interface.
 * @version 1.8.8
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/interfaces/i_storage_backend.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

/**
 * @brief Generate a temp path for the C interface tests.
 * @return Path string.
 * @utility
 * @version 1.8.8
 */
static std::string temp_c_db_path() {
    auto dir = fs::temp_directory_path() / "entropic_test";
    fs::create_directories(dir);
    return (dir / "c_test.db").string();
}

/**
 * @brief RAII wrapper for C storage handle.
 * @internal
 * @version 1.8.8
 */
struct CStorage {
    std::string path;
    entropic_storage_backend_t handle;

    /**
     * @brief Construct and initialize.
     * @version 1.8.8
     */
    CStorage() : path(temp_c_db_path()) {
        handle = entropic_storage_create(path.c_str());
        entropic_storage_initialize(handle);
    }

    /**
     * @brief Destructor — destroy handle and remove file.
     * @version 1.8.8
     */
    ~CStorage() {
        entropic_storage_destroy(handle);
        fs::remove(path);
    }
};

SCENARIO("C API handle lifecycle", "[storage][c_interface]") {
    GIVEN("A storage handle") {
        auto path = temp_c_db_path();

        WHEN("created and initialized") {
            auto* h = entropic_storage_create(path.c_str());
            auto err = entropic_storage_initialize(h);

            THEN("it succeeds") {
                REQUIRE(h != nullptr);
                REQUIRE(err == ENTROPIC_OK);
            }

            entropic_storage_destroy(h);
            fs::remove(path);
        }
    }
}

SCENARIO("C API NULL handle returns error",
         "[storage][c_interface]") {
    GIVEN("A NULL storage handle") {
        WHEN("any operation is attempted") {
            THEN("it returns ENTROPIC_ERROR_INVALID_ARGUMENT") {
                REQUIRE(entropic_storage_initialize(nullptr)
                        == ENTROPIC_ERROR_INVALID_ARGUMENT);
                REQUIRE(entropic_storage_save_conversation(
                    nullptr, "id", "[]")
                        == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }

    GIVEN("A NULL destroy call") {
        WHEN("destroy is called with NULL") {
            THEN("it does not crash") {
                entropic_storage_destroy(nullptr);
            }
        }
    }
}

SCENARIO("C API full conversation lifecycle",
         "[storage][c_interface]") {
    GIVEN("An initialized C storage handle") {
        CStorage cs;

        WHEN("a conversation is created, saved, and loaded") {
            char* conv_id = entropic_storage_create_conversation(
                cs.handle, "C API Test", nullptr, nullptr);
            REQUIRE(conv_id != nullptr);

            auto err = entropic_storage_save_conversation(
                cs.handle, conv_id,
                R"([{"role":"user","content":"Hello from C"}])");
            REQUIRE(err == ENTROPIC_OK);

            char* result = nullptr;
            err = entropic_storage_load_conversation(
                cs.handle, conv_id, &result);

            THEN("the conversation loads successfully") {
                REQUIRE(err == ENTROPIC_OK);
                REQUIRE(result != nullptr);
                auto j = json::parse(result);
                REQUIRE(j["messages"].size() == 1);
                REQUIRE(j["messages"][0]["content"] == "Hello from C");
            }

            std::free(result);
            std::free(conv_id);
        }
    }
}

SCENARIO("C API delegation round-trip",
         "[storage][c_interface]") {
    GIVEN("A parent conversation via C API") {
        CStorage cs;
        char* parent_id = entropic_storage_create_conversation(
            cs.handle, "Parent", nullptr, nullptr);

        WHEN("a delegation is created and completed") {
            char* del_result = nullptr;
            auto err = entropic_storage_create_delegation(
                cs.handle, parent_id, "lead", "eng",
                "Build feature", 5, &del_result);

            THEN("delegation is created") {
                REQUIRE(err == ENTROPIC_OK);
                auto j = json::parse(del_result);
                auto del_id = j["delegation_id"].get<std::string>();
                REQUIRE(del_id.size() == 36);

                err = entropic_storage_complete_delegation(
                    cs.handle, del_id.c_str(),
                    "completed", "All done");
                REQUIRE(err == ENTROPIC_OK);
            }

            std::free(del_result);
        }

        std::free(parent_id);
    }
}

SCENARIO("C API snapshot and stats",
         "[storage][c_interface]") {
    GIVEN("A conversation with messages") {
        CStorage cs;
        char* conv_id = entropic_storage_create_conversation(
            cs.handle, "Stats test", nullptr, nullptr);
        entropic_storage_save_conversation(cs.handle, conv_id,
            R"([{"role":"user","content":"test","token_count":42}])");

        WHEN("a snapshot is saved") {
            auto err = entropic_storage_save_snapshot(
                cs.handle, conv_id,
                R"([{"role":"user","content":"test"}])");

            THEN("it succeeds") {
                REQUIRE(err == ENTROPIC_OK);
            }
        }

        WHEN("stats are queried") {
            char* stats = nullptr;
            auto err = entropic_storage_get_stats(cs.handle, &stats);

            THEN("stats are returned") {
                REQUIRE(err == ENTROPIC_OK);
                auto j = json::parse(stats);
                REQUIRE(j["total_conversations"] == 1);
                REQUIRE(j["total_messages"] == 1);
            }
            std::free(stats);
        }

        std::free(conv_id);
    }
}

SCENARIO("C API search", "[storage][c_interface]") {
    GIVEN("A conversation with searchable content") {
        CStorage cs;
        char* conv_id = entropic_storage_create_conversation(
            cs.handle, "Searchable", nullptr, nullptr);
        entropic_storage_save_conversation(cs.handle, conv_id,
            R"([{"role":"user","content":"unique_search_term_xyz"}])");

        WHEN("searching via FTS5") {
            char* result = nullptr;
            auto err = entropic_storage_search_conversations(
                cs.handle, "unique_search_term_xyz", 10, &result);

            THEN("results include the conversation") {
                REQUIRE(err == ENTROPIC_OK);
                auto arr = json::parse(result);
                REQUIRE(arr.size() >= 1);
            }
            std::free(result);
        }

        std::free(conv_id);
    }
}
