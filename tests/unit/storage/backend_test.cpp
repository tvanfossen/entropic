// SPDX-License-Identifier: Apache-2.0
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

SCENARIO("create_delegation refuses empty parent (gh#48)",
         "[storage][backend][gh48]") {
    GIVEN("a storage backend with NO parent conversation") {
        TempStorage t;
        WHEN("create_delegation is called with an empty parent_id") {
            std::string del_id = "preset", child_id = "preset";
            bool ok = t.storage.create_delegation(
                "", "lead", "eng", "Implement feature", 10,
                del_id, child_id);
            THEN("it returns false and clears the out params") {
                // Pre-v2.1.12 this returned true (sort of) — the SQL
                // INSERT FK-failed silently, the bool propagated false
                // but the unconditional "Created delegation" log
                // masked the failure. The defense-in-depth fix
                // rejects an empty parent up front with a clear error.
                REQUIRE_FALSE(ok);
                REQUIRE(del_id.empty());
                REQUIRE(child_id.empty());
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

// ── v2.3.10: coverage for delegation lookup + search ──

SCENARIO("get_delegation_by_id retrieves a single delegation row",
         "[storage][backend][v2.3.10][coverage]") {
    TempStorage t;
    auto parent = t.storage.create_conversation("parent-2310");
    std::string del_id, child;
    REQUIRE(t.storage.create_delegation(
        parent, "lead", "eng", "task-2310", 3, del_id, child));
    t.storage.complete_delegation(del_id, "completed", "row-summary");

    WHEN("get_delegation_by_id is called with the known id") {
        std::string out;
        bool found = t.storage.get_delegation_by_id(del_id, out);

        THEN("the row is found and JSON serialized") {
            REQUIRE(found);
            auto j = json::parse(out);
            REQUIRE(j["id"] == del_id);
            REQUIRE(j["status"] == "completed");
        }
    }

    WHEN("get_delegation_by_id is called with an unknown id") {
        std::string out;
        bool found = t.storage.get_delegation_by_id(
            "no-such-delegation", out);
        THEN("it returns false (no exception)") {
            REQUIRE_FALSE(found);
        }
    }
}

SCENARIO("search_delegations finds completed rows by substring",
         "[storage][backend][v2.3.10][coverage]") {
    TempStorage t;
    auto parent = t.storage.create_conversation("search-parent");
    std::string del_id, child;
    REQUIRE(t.storage.create_delegation(
        parent, "lead", "eng", "do thing", 5, del_id, child));
    t.storage.complete_delegation(
        del_id, "completed", "found-the-magic-token");

    WHEN("search_delegations finds the matching summary") {
        std::string out;
        bool ok = t.storage.search_delegations(
            "magic-token", 5, out);
        THEN("at least one row returns") {
            REQUIRE(ok);
            auto arr = json::parse(out);
            REQUIRE(arr.size() >= 1);
            REQUIRE(arr[0]["result_summary"]
                    .get<std::string>().find("magic-token")
                    != std::string::npos);
        }
    }

    WHEN("search_delegations finds nothing") {
        std::string out;
        bool ok = t.storage.search_delegations(
            "missing-substring-xyz", 5, out);
        THEN("the result is an empty JSON array") {
            REQUIRE(ok);
            auto arr = json::parse(out);
            REQUIRE(arr.empty());
        }
    }
}

// ── gh#112 / gh#113 (v2.9.9): storage read boundary sanitizes UTF-8 ──────────
//
// Root cause of the type_error.316 family: MTP can split a multi-byte UTF-8
// codepoint at a token boundary, producing an invalid byte sequence. That
// sequence flows into DelegationResult.summary and is stored verbatim in
// delegations.result_summary via complete_delegation (sqlite3_bind_text, no
// validation). On the read side, delegation_row_to_json does a direct
// assignment entry["result_summary"] = col_text(...); the subsequent
// entry.dump() in get_delegation_by_id throws type_error.316.
//
// Note: the messages table is NOT a viable attack surface — save_messages
// calls json::parse which rejects strings containing invalid UTF-8, so
// message_row_to_json / load_conversation cannot see bad bytes.
//
// RED/GREEN protocol (verified manually, see commit message):
//   RED:   current code — entry["result_summary"] = col_opt_text(s, 8)...
//          (no sanitize). get_delegation_by_id → entry.dump() throws 316.
//   GREEN: fix applied — sanitize at read boundary in delegation_row_to_json.
//          All assertions pass; bad bytes replaced with U+FFFD.

SCENARIO("gh#112/gh#113: get_delegation_by_id sanitizes invalid UTF-8 in "
         "result_summary at the storage read boundary (v2.9.9 permanent fix)",
         "[storage][backend][gh112][gh113][utf8][regression][2.9.9]") {
    // The exact bytes from the production gh#113 crash: MTP split a 3-byte
    // CJK codepoint. First two bytes (0xE6 0x9E) survived; the expected
    // third continuation byte was replaced by ASCII '.' (0x2E).
    const std::string kSplitCjk =
        std::string(125, 'a') + "\xE6\x9E\x2E" + " rest of summary";
    const std::string kLoneContinuation =
        std::string("result: ") + "\x80" + " done";
    const std::string kTruncatedAtEnd =
        std::string("truncated ") + "\xE4\xB8";  // 3-byte lead, only 1 cont

    GIVEN("a delegation whose result_summary has a split MTP codepoint") {
        TempStorage t;
        auto parent = t.storage.create_conversation("gh112 split-cjk parent");
        std::string del_id, child;
        REQUIRE(t.storage.create_delegation(
            parent, "lead", "researcher", "research task", 0, del_id, child));
        // complete_delegation stores via sqlite3_bind_text — no UTF-8 check.
        // This is the exact path from the production crash.
        t.storage.complete_delegation(del_id, "completed", kSplitCjk);

        WHEN("get_delegation_by_id reads the row back") {
            std::string out;
            bool found = t.storage.get_delegation_by_id(del_id, out);

            THEN("it succeeds (does not throw type_error.316)") {
                REQUIRE(found);
                REQUIRE_NOTHROW(json::parse(out));
            }
            AND_THEN("the JSON round-trips through dump without throwing") {
                REQUIRE(found);
                REQUIRE_NOTHROW(json::parse(out).dump());
            }
            AND_THEN("result_summary is valid UTF-8 (bad bytes replaced)") {
                REQUIRE(found);
                auto j = json::parse(out);
                nlohmann::json probe;
                probe["summary"] = j["result_summary"].get<std::string>();
                REQUIRE_NOTHROW(probe.dump());
            }
            AND_THEN("clean fields (id, status) are not mangled") {
                REQUIRE(found);
                auto j = json::parse(out);
                CHECK(j["id"] == del_id);
                CHECK(j["status"] == "completed");
            }
        }
    }

    GIVEN("a delegation result_summary with a lone continuation byte") {
        TempStorage t;
        auto parent = t.storage.create_conversation("gh112 lone-cont parent");
        std::string del_id, child;
        REQUIRE(t.storage.create_delegation(
            parent, "lead", "researcher", "task", 0, del_id, child));
        t.storage.complete_delegation(del_id, "completed", kLoneContinuation);

        WHEN("loaded via get_delegation_by_id") {
            std::string out;
            bool found = t.storage.get_delegation_by_id(del_id, out);
            THEN("dump does not throw type_error.316") {
                REQUIRE(found);
                REQUIRE_NOTHROW(json::parse(out).dump());
            }
        }
    }

    GIVEN("a delegation result_summary with a truncated multi-byte sequence") {
        TempStorage t;
        auto parent = t.storage.create_conversation("gh112 truncated parent");
        std::string del_id, child;
        REQUIRE(t.storage.create_delegation(
            parent, "lead", "researcher", "task", 0, del_id, child));
        t.storage.complete_delegation(del_id, "completed", kTruncatedAtEnd);

        WHEN("loaded via get_delegation_by_id") {
            std::string out;
            bool found = t.storage.get_delegation_by_id(del_id, out);
            THEN("dump does not throw type_error.316") {
                REQUIRE(found);
                REQUIRE_NOTHROW(json::parse(out).dump());
            }
        }
    }
}

// ── gh#114 (v2.9.11): task field also needs UTF-8 sanitize at read boundary ──
//
// gh#112 sanitized result_summary but left the task column unsanitized.
// The task description is model-generated (MTP-enabled runs produce it via
// a delegation tool-call); split codepoints land in delegations.task via
// create_delegation → bind_delegation_insert → sqlite3_bind_text (no check).
// On the read side, delegation_row_to_json returns
//   entry["task"] = col_text(s, 5)     // ← raw, no sanitize
// and the caller's arr.dump() / entry.dump() throws type_error.316.
//
// Consumer symptom: crash after "follow-up recall party on accumulated stores"
// — engine calls get_delegations / search_delegations to recall prior work
// after a multi-delegation session where MTP split a codepoint in a task field.
//
// RED/GREEN protocol:
//   RED:   unfixed code — get_delegations / get_delegation_by_id / search_delegations
//          all throw type_error.316 when the stored task has bad UTF-8.
//   GREEN: fix applied — sanitize_storage_utf8() on task at read boundary in
//          both delegation_row_to_json and delegation_summary_to_json; bad bytes
//          replaced with U+FFFD replacement character.

SCENARIO("gh#114: get_delegations sanitizes invalid UTF-8 in task field "
         "at the storage read boundary (v2.9.11 permanent fix)",
         "[storage][backend][gh114][utf8][regression][2.9.11]") {
    // Same MTP split-codepoint pattern as gh#112/gh#113: 3-byte CJK sequence
    // with the final continuation byte replaced by ASCII '.' — exact shape of
    // what MTP emits when a Chinese/Japanese token is split at a batch boundary.
    const std::string kSplitTask =
        std::string("Summarise the ") + "\xE6\x9E\x2E" + " module findings";
    const std::string kLoneContTask =
        std::string("Analyse results\x80 of run");
    const std::string kTruncatedTask =
        std::string("Compare with baseline \xE4\xB8");

    GIVEN("a delegation whose task has a split MTP codepoint in the description") {
        TempStorage t;
        auto parent = t.storage.create_conversation("gh114 split-task parent");
        std::string del_id, child;
        // create_delegation stores task via sqlite3_bind_text — no UTF-8 check.
        // This is the production path: engine builds a delegation tool-call
        // with MTP-produced task text and calls create_delegation.
        REQUIRE(t.storage.create_delegation(
            parent, "lead", "researcher", kSplitTask, 0, del_id, child));
        t.storage.complete_delegation(del_id, "completed", "clean summary");

        WHEN("get_delegations reads all delegations for the parent") {
            std::string out;
            bool ok = t.storage.get_delegations(parent, out);

            THEN("it succeeds without throwing type_error.316") {
                REQUIRE(ok);
                REQUIRE_NOTHROW(json::parse(out).dump());
            }
            AND_THEN("task field is valid UTF-8 (bad bytes replaced)") {
                REQUIRE(ok);
                auto arr = json::parse(out);
                REQUIRE(arr.size() == 1);
                nlohmann::json probe;
                probe["task"] = arr[0]["task"].get<std::string>();
                REQUIRE_NOTHROW(probe.dump());
            }
            AND_THEN("clean fields (id, status) are not mangled") {
                REQUIRE(ok);
                auto arr = json::parse(out);
                CHECK(arr[0]["id"] == del_id);
                CHECK(arr[0]["status"] == "completed");
            }
        }

        WHEN("get_delegation_by_id reads the same row") {
            std::string out;
            bool found = t.storage.get_delegation_by_id(del_id, out);
            THEN("dump does not throw type_error.316") {
                REQUIRE(found);
                REQUIRE_NOTHROW(json::parse(out).dump());
            }
        }
    }

    GIVEN("a delegation task with a lone continuation byte") {
        TempStorage t;
        auto parent = t.storage.create_conversation("gh114 lone-cont-task parent");
        std::string del_id, child;
        REQUIRE(t.storage.create_delegation(
            parent, "lead", "researcher", kLoneContTask, 0, del_id, child));

        WHEN("get_delegations is called") {
            std::string out;
            bool ok = t.storage.get_delegations(parent, out);
            THEN("dump does not throw type_error.316") {
                REQUIRE(ok);
                REQUIRE_NOTHROW(json::parse(out).dump());
            }
        }
    }

    GIVEN("a delegation task with a truncated multi-byte sequence") {
        TempStorage t;
        auto parent = t.storage.create_conversation("gh114 truncated-task parent");
        std::string del_id, child;
        REQUIRE(t.storage.create_delegation(
            parent, "lead", "researcher", kTruncatedTask, 0, del_id, child));
        // search_delegations finds rows by result_summary; give it a clean one
        t.storage.complete_delegation(del_id, "completed", "clean-searchable-summary");

        WHEN("search_delegations returns the row (bad task, clean summary)") {
            std::string out;
            bool ok = t.storage.search_delegations("clean-searchable-summary", 5, out);
            THEN("dump does not throw type_error.316") {
                REQUIRE(ok);
                auto arr = json::parse(out);
                REQUIRE(arr.size() >= 1);
                REQUIRE_NOTHROW(arr.dump());
            }
            AND_THEN("task field is valid UTF-8") {
                REQUIRE(ok);
                auto arr = json::parse(out);
                nlohmann::json probe;
                probe["task"] = arr[0]["task"].get<std::string>();
                REQUIRE_NOTHROW(probe.dump());
            }
        }
    }
}
