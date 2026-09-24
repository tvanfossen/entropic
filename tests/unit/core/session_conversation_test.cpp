// SPDX-License-Identifier: Apache-2.0
/**
 * @file session_conversation_test.cpp
 * @brief gh#144: two callers on one engine must not share a history.
 *
 * The property is the one stated by the multi-handle isolation test one layer
 * up — "two handles interleaving turns... the last writer must not win
 * globally" — applied to sessions on a single handle. Against v2.11.1 the
 * engine held ONE conversation vector, so session B's turn appended to
 * session A's history and each read the other's context.
 *
 * CPU-only against MockInference: keying is engine state, and needs no model.
 *
 * @version 2.12.0
 */

#include <entropic/core/engine.h>
#include "mock_inference.h"
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace entropic;
using namespace entropic::test;

SCENARIO("gh#144: interleaved sessions keep disjoint histories",
         "[engine][gh144][session][2.12.0]") {
    GIVEN("one engine and two callers") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        WHEN("they interleave turns A, B, A, B") {
            engine.set_active_session("repo-a");
            engine.run_turn("alpha one");
            engine.set_active_session("repo-b");
            engine.run_turn("bravo one");
            engine.set_active_session("repo-a");
            engine.run_turn("alpha two");
            engine.set_active_session("repo-b");
            engine.run_turn("bravo two");

            THEN("each session holds only its own turns") {
                auto a = engine.messages_for("repo-a");
                auto b = engine.messages_for("repo-b");

                bool a_has_bravo = false;
                for (const auto& m : a) {
                    if (m.content.find("bravo") != std::string::npos) {
                        a_has_bravo = true;
                    }
                }
                bool b_has_alpha = false;
                for (const auto& m : b) {
                    if (m.content.find("alpha") != std::string::npos) {
                        b_has_alpha = true;
                    }
                }
                // Against the single shared vector both of these were true:
                // the last writer won globally.
                CHECK_FALSE(a_has_bravo);
                CHECK_FALSE(b_has_alpha);
            }

            AND_THEN("both accumulated their own two turns") {
                CHECK(engine.message_count_for("repo-a")
                      == engine.message_count_for("repo-b"));
                CHECK(engine.message_count_for("repo-a") >= 4);
            }

            AND_THEN("the default session was never touched") {
                CHECK(engine.message_count_for("") == 0);
            }
        }
    }
}

SCENARIO("gh#144: the default session behaves exactly as before",
         "[engine][gh144][regression][2.12.0]") {
    GIVEN("an engine driven without ever naming a session") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        WHEN("two turns run through the legacy entry point") {
            engine.run_turn("first");
            engine.run_turn("second");

            THEN("the legacy accessors see them, unchanged") {
                // get_messages() still returns a REFERENCE into the store.
                // Every pre-2.12.0 caller depends on that, which is why the
                // "" entry is constructed in the ctor and never erased.
                const auto& msgs = engine.get_messages();
                CHECK(msgs.size() == engine.message_count());
                CHECK(engine.message_count() >= 4);
            }
            AND_THEN("the keyed accessors agree with them") {
                CHECK(engine.messages_for("").size()
                      == engine.message_count());
            }
        }
    }
}

SCENARIO("gh#144: clearing one session leaves the others intact",
         "[engine][gh144][session][2.12.0]") {
    GIVEN("two sessions with history") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        engine.set_active_session("a");
        engine.run_turn("one");
        engine.set_active_session("b");
        engine.run_turn("two");
        REQUIRE(engine.message_count_for("a") > 0);

        WHEN("one is cleared by name") {
            engine.clear_conversation_for("a");

            THEN("only that one is empty") {
                // entropic.context_clear used to wipe every caller's
                // context, which is why the issue said it "is not
                // isolation" — with two callers active there was no
                // ordering in which it was correct.
                CHECK(engine.message_count_for("a") == 0);
                CHECK(engine.message_count_for("b") > 0);
            }
        }

        WHEN("one is dropped entirely") {
            REQUIRE(engine.drop_session("a"));

            THEN("it is gone and the other survives") {
                CHECK(engine.message_count_for("a") == 0);
                CHECK(engine.message_count_for("b") > 0);
            }
        }

        WHEN("the default session is dropped") {
            THEN("it is cleared rather than erased, so accessors stay total") {
                // Note get_messages() is ACTIVE-session relative, not
                // default-relative — the active session here is still "b".
                // Reading it after dropping "" would assert the wrong thing.
                CHECK(engine.drop_session(""));
                CHECK(engine.messages_for("").empty());
                // And the accessor is still total: "" was cleared, not
                // erased, so this cannot throw.
                engine.set_active_session("");
                CHECK(engine.get_messages().empty());
            }
        }
    }
}

SCENARIO("gh#144: a tier switch seeds the new session's own system prompt",
         "[engine][gh144][gh99][2.12.0]") {
    GIVEN("an engine whose default session has already run") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        engine.set_system_prompt("PROMPT-ONE");
        engine.run_turn("hello");
        REQUIRE(engine.message_count() > 0);

        WHEN("a fresh session starts under a different prompt") {
            engine.set_system_prompt("PROMPT-TWO");
            engine.set_active_session("second");
            engine.run_turn("hello again");

            THEN("it is seeded with the CURRENT prompt, not the first one") {
                // Retires the IOU in seed_system_prompt_for_tier: "the system
                // prompt is NOT re-seeded mid-session when a later
                // run_turn_as names a different tier". With a per-session
                // conversation there is a place to put it.
                auto msgs = engine.messages_for("second");
                REQUIRE_FALSE(msgs.empty());
                CHECK(msgs.front().role == "system");
                CHECK(msgs.front().content == "PROMPT-TWO");
            }
            AND_THEN("the first session kept its own") {
                auto first = engine.messages_for("");
                REQUIRE_FALSE(first.empty());
                CHECK(first.front().content == "PROMPT-ONE");
            }
        }
    }
}

// ── gh#165: restoring a session's conversation (v2.13.0) ──────────────

SCENARIO("gh#165: a conversation can be replaced wholesale",
         "[engine][gh165][session][2.13.0]") {
    GIVEN("a session with history") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        engine.set_active_session("repo-a");
        engine.run_turn("alpha one");
        REQUIRE(engine.message_count_for("repo-a") > 0);

        WHEN("a stored conversation is restored over it") {
            std::vector<Message> restored;
            Message sys;
            sys.role = "system";
            sys.content = "restored prompt";
            Message user;
            user.role = "user";
            user.content = "restored turn";
            user.metadata["tool_name"] = "filesystem.read";
            restored = {sys, user};

            REQUIRE(engine.set_session_messages("repo-a", restored));

            THEN("the session holds exactly what was restored") {
                auto msgs = engine.messages_for("repo-a");
                REQUIRE(msgs.size() == 2);
                CHECK(msgs[0].content == "restored prompt");
                CHECK(msgs[1].content == "restored turn");
            }
            AND_THEN("metadata came with it") {
                auto msgs = engine.messages_for("repo-a");
                REQUIRE(msgs.size() == 2);
                CHECK(msgs[1].metadata.at("tool_name") == "filesystem.read");
            }
        }
    }
}

SCENARIO("gh#165: the RUNNING session refuses replacement, others do not",
         "[engine][gh165][session][2.13.0]") {
    GIVEN("a run in flight on session A, with concurrency enabled") {
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        engine.set_concurrent_sessions(true);

        // The session APIs take api_mutex but NOT the run guard, so
        // clear/drop could already mutate conversations_ mid-turn — a latent
        // race since v2.12.0 that a write counterpart would turn into a
        // likely one.
        REQUIRE(engine.try_begin_turn("repo-a"));

        WHEN("A's conversation is replaced") {
            std::vector<Message> m(1);
            m[0].role = "user";
            m[0].content = "replacement";

            THEN("it is refused — a turn is appending to that vector") {
                CHECK_FALSE(engine.set_session_messages("repo-a", m));
            }
            AND_THEN("a DIFFERENT session is still replaceable mid-run") {
                // Busy HANDLE is not the rule; busy CONVERSATION is.
                CHECK(engine.set_session_messages("repo-b", m));
                CHECK(engine.message_count_for("repo-b") == 1);
            }
        }

        engine.end_turn("repo-a");

        WHEN("the run finishes") {
            std::vector<Message> m(1);
            m[0].role = "user";
            m[0].content = "replacement";

            THEN("A is replaceable again") {
                CHECK(engine.set_session_messages("repo-a", m));
            }
        }
    }
}

// ── gh#158 (v2.13.0): the unkeyed accessors must be TOTAL ──────────────

SCENARIO("gh#158: the unkeyed accessors survive a key that is not in the map",
         "[engine][gh158][session][2.13.0]") {
    GIVEN("an engine whose active session has been dropped") {
        // `get_messages()` and `message_count()` resolved their key and then
        // indexed `conversations_` with `.at()`. `.at()` on an absent key
        // throws `std::out_of_range("_Map_base::at")`, and both accessors are
        // reached from the C ABI — `entropic_context_usage`,
        // `entropic_get_messages` — where an escaping exception is a
        // std::terminate, i.e. the HOST process dies.
        //
        // Two ordinary sequences reach an absent key. This is the one a
        // single thread can produce: drop the session that is still the
        // active one. The concurrent one (a run publishing its key before
        // the entry exists) is in tests/concurrency/test_thread_safety.cpp,
        // and it is what aborted the v2.13.0 gh#158 GPU gate:
        //
        //     terminate called after throwing an instance of
        //       'std::out_of_range'  what():  _Map_base::at
        //     ... SIGABRT - Abort (abnormal termination) signal
        MockInference mock;
        auto iface = make_mock_interface(mock);
        LoopConfig lc;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        engine.set_active_session("repo-a");
        engine.run_turn("alpha one");
        REQUIRE(engine.message_count() > 0);
        REQUIRE(engine.drop_session("repo-a"));

        WHEN("a host reads context without naming a session") {
            THEN("the count answers for the default session, it does not "
                 "throw") {
                CHECK(engine.message_count() == 0);
            }
            AND_THEN("the message accessor answers too") {
                CHECK(engine.get_messages().empty());
            }
        }
    }
}
