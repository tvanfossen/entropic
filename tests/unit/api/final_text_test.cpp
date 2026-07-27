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
