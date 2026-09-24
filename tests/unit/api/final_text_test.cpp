// SPDX-License-Identifier: Apache-2.0
/**
 * @file final_text_test.cpp
 * @brief gh#130: the bridge must not answer "(no response)" when the
 *        conversation already holds the answer.
 *
 * Consumer symptom (entropic-engine v2.9.20, `entropic.ask` over the
 * ExternalBridge socket): a turn that ends without `entropic.complete` leaves
 * a trailing EMPTY assistant message. `extract_final_text` returned that empty
 * content and stopped scanning, so the operator got the literal string
 * "(no response)" — in ~4 of 16 runs of a live acceptance matrix, including
 * one where a sub-tier delegation had already produced the correct citable
 * answer.
 *
 * The fixtures use an R"JSON(...)JSON" delimiter because the payloads contain
 * the sequence `)"`, which would close a plain R"(...)" literal early.
 *
 * @version 2.10.2
 */

#include "final_text.h"  // private facade header

#include <catch2/catch_test_macros.hpp>

#include <string>

using facade_text::extract_final_text;
using facade_text::final_text_or_reason;
using facade_text::no_response_reason;

SCENARIO("gh#130 a trailing empty assistant turn does not hide the answer",
         "[facade][external_bridge][gh130]")
{
    GIVEN("prose, then a terminal empty assistant turn from an anti-spiral stop") {
        // Exactly the reported shape: the lead answers, its next tool call is
        // rejected by anti-spiral, and the following generation returns
        // finish=stop with 0 tool calls and 0 chars.
        const char* convo = R"JSON([
            {"role":"user","content":"What does BRULE-028 limit?"},
            {"role":"assistant","content":"BRULE-028 limits returns to 3."},
            {"role":"user","content":"(engine: tool rejected - anti-spiral)"},
            {"role":"assistant","content":""}
        ])JSON";

        WHEN("the final text is extracted") {
            auto text = extract_final_text(convo);

            THEN("the earlier prose is returned, not the empty turn") {
                CHECK(text == "BRULE-028 limits returns to 3.");
            }

            THEN("the operator does not receive a no-response sentinel") {
                CHECK(final_text_or_reason(convo).rfind("(no response", 0)
                      != 0);
            }
        }
    }

    GIVEN("a folded delegation summary shadowed by a later empty turn") {
        // The worst observed case. fold_delegation_summary (gh#119, v2.9.17)
        // puts the child's summary into the lead's empty assistant turn so
        // this function can find it - but the loop then produced ANOTHER
        // empty assistant turn, which shadowed it.
        const char* convo = R"JSON([
            {"role":"user","content":"What does BRULE-028 limit?"},
            {"role":"assistant","content":""},
            {"role":"user","content":"[DELEGATION COMPLETE] researcher"},
            {"role":"assistant","content":"BRULE-028 limits the number of return statements per function to 3, as specified in sops/sop-005-module-compliance.md"},
            {"role":"user","content":"(engine: tool rejected - anti-spiral)"},
            {"role":"assistant","content":""}
        ])JSON";

        WHEN("the final text is extracted") {
            auto text = extract_final_text(convo);

            THEN("the delegation answer survives") {
                CHECK(text.find("sop-005-module-compliance.md")
                      != std::string::npos);
            }
        }
    }

    GIVEN("a normal turn ending in real assistant content") {
        const char* convo = R"JSON([
            {"role":"user","content":"hi"},
            {"role":"assistant","content":"hello"}
        ])JSON";

        WHEN("the final text is extracted") {
            THEN("the last message still wins - no regression") {
                CHECK(extract_final_text(convo) == "hello");
            }
        }
    }

    GIVEN("an earlier assistant turn followed by a newer non-empty one") {
        const char* convo = R"JSON([
            {"role":"assistant","content":"stale"},
            {"role":"user","content":"more"},
            {"role":"assistant","content":"fresh"}
        ])JSON";

        WHEN("the final text is extracted") {
            THEN("recency still wins over the older message") {
                CHECK(extract_final_text(convo) == "fresh");
            }
        }
    }
}

SCENARIO("gh#130 a user message is never mistaken for the answer",
         "[facade][external_bridge][gh130]")
{
    // Tool AND delegation results are injected as role:"user"
    // (tool_executor.cpp), and serialize_messages emits only {role, content},
    // so at this layer they are indistinguishable from the operator's own
    // prompt. Echoing one back as the answer is worse than admitting there
    // is none.
    GIVEN("a conversation whose only content lives in user messages") {
        const char* convo = R"JSON([
            {"role":"user","content":"What does BRULE-028 limit?"},
            {"role":"assistant","content":""}
        ])JSON";

        WHEN("the final text is extracted") {
            THEN("nothing is returned rather than the user's own prompt") {
                auto text = extract_final_text(convo);
                CHECK(text.empty());
                CHECK(text.find("BRULE-028") == std::string::npos);
            }
        }
    }
}

SCENARIO("gh#130 the empty-answer case explains itself",
         "[facade][external_bridge][gh130]")
{
    GIVEN("a conversation with assistant turns that are all empty") {
        const char* convo = R"JSON([
            {"role":"user","content":"q"},
            {"role":"assistant","content":""}
        ])JSON";

        WHEN("the reason is requested") {
            auto reason = no_response_reason(convo);

            THEN("it names the stall rather than being a bare sentinel") {
                CHECK(reason.find("entropic.complete") != std::string::npos);
            }

            THEN("it stays substring-compatible with the old sentinel") {
                CHECK(reason.rfind("(no response", 0) == 0);
            }
        }
    }

    GIVEN("a conversation with no assistant message at all") {
        const char* convo = R"JSON([{"role":"user","content":"q"}])JSON";

        WHEN("the reason is requested") {
            THEN("it is distinguishable from the all-empty case") {
                auto reason = no_response_reason(convo);
                CHECK(reason.find("no assistant message") != std::string::npos);
            }
        }
    }

    GIVEN("unparseable engine output") {
        WHEN("the reason is requested") {
            THEN("a null pointer is reported, not crashed on") {
                CHECK(no_response_reason(nullptr).find("no readable")
                      != std::string::npos);
                CHECK(extract_final_text(nullptr).empty());
            }

            THEN("malformed JSON is reported, not crashed on") {
                CHECK(no_response_reason("{not json").find("no readable")
                      != std::string::npos);
                CHECK(extract_final_text("{not json").empty());
            }
        }
    }
}

SCENARIO("gh#165 a recall assertion must read the answer, not the transcript",
         "[facade][gh165][model-harness]")
{
    // Why this lives here rather than in tests/model: the model suite is
    // GPU-gated, so the proof that its recall assertions CAN fail has to be
    // runnable on CPU. `extract_final_text` is the exact function
    // facade_model_helpers.h's `final_answer` calls, so what is pinned here
    // is what the model tests now assert on.
    //
    // The defect: `entropic_run_session` returns
    // `serialize_messages(engine->run_turn(input))` — the WHOLE conversation.
    // A model test that seeded "cinnamon" and then asserted
    // `contains_ci(returned_string, "cinnamon")` was matching its own seed
    // message. A v2.13.0 gate run of test_gh165_session_restore reported 28
    // passing assertions while every turn decoded 0 characters.

    GIVEN("a turn in which the model decoded nothing at all") {
        // Exactly the gate-log shape: both assistant turns came back empty.
        const char* convo = R"JSON([
            {"role":"system","content":"You are a terse assistant."},
            {"role":"user","content":"Remember this word, I will ask for it later: cinnamon"},
            {"role":"assistant","content":""},
            {"role":"user","content":"Repeat that exact word now, and nothing else."},
            {"role":"assistant","content":""}
        ])JSON";

        WHEN("the OLD form asks whether the returned string holds the secret") {
            THEN("it does — the test wrote it there itself, so it cannot fail") {
                CHECK(std::string(convo).find("cinnamon") != std::string::npos);
            }
        }

        WHEN("the NEW form asks whether the ANSWER holds the secret") {
            const auto answer = extract_final_text(convo);

            THEN("there is no answer at all") {
                CHECK(answer.empty());
            }

            THEN("the recall assertion is now capable of failing") {
                CHECK(answer.find("cinnamon") == std::string::npos);
            }
        }
    }

    GIVEN("a turn whose only assistant text is the seed acknowledgement") {
        // The subtler case: the seed turn answered, the recall turn did not.
        // The transcript holds BOTH the secret and assistant prose, so a
        // non-empty check would not have caught this either.
        const char* convo = R"JSON([
            {"role":"system","content":"You are a terse assistant."},
            {"role":"user","content":"Remember this word, I will ask for it later: cinnamon"},
            {"role":"assistant","content":"Understood, I will remember it."},
            {"role":"user","content":"Repeat that exact word now, and nothing else."},
            {"role":"assistant","content":""}
        ])JSON";

        WHEN("the answer is extracted") {
            const auto answer = extract_final_text(convo);

            THEN("it is the acknowledgement, which is not a recall") {
                CHECK(answer == "Understood, I will remember it.");
                CHECK(answer.find("cinnamon") == std::string::npos);
            }
        }
    }

    GIVEN("a turn in which the model DID recall the word") {
        // The other half of the proof: the assertion must still PASS when
        // the model does what the test asks, or it would just be inverted.
        const char* convo = R"JSON([
            {"role":"system","content":"You are a terse assistant."},
            {"role":"user","content":"Remember this word, I will ask for it later: cinnamon"},
            {"role":"assistant","content":"Understood, I will remember it."},
            {"role":"user","content":"Repeat that exact word now, and nothing else."},
            {"role":"assistant","content":"cinnamon"}
        ])JSON";

        WHEN("the answer is extracted") {
            const auto answer = extract_final_text(convo);

            THEN("the recall assertion passes on the model's own words") {
                CHECK(answer == "cinnamon");
            }
        }
    }

    GIVEN("a turn whose last assistant message answers a DIFFERENT question") {
        // gh#158/gh#166 shape: the negative assertion stays on the
        // transcript (a cross-session read lands in the history whether or
        // not the model repeats it), the positive one moves to the answer.
        const char* convo = R"JSON([
            {"role":"user","content":"Remember this word, I will ask for it later: cinnamon"},
            {"role":"assistant","content":"Understood."},
            {"role":"user","content":"List two colours, nothing else."},
            {"role":"assistant","content":"Red and blue."},
            {"role":"user","content":"Repeat that exact word now, and nothing else."},
            {"role":"assistant","content":"Red and blue."}
        ])JSON";

        WHEN("the answer is extracted") {
            const auto answer = extract_final_text(convo);

            THEN("the filler answer is caught, where the transcript hid it") {
                CHECK(answer == "Red and blue.");
                CHECK(answer.find("cinnamon") == std::string::npos);
                CHECK(std::string(convo).find("cinnamon")
                      != std::string::npos);
            }
        }
    }
}
