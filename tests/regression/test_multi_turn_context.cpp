/**
 * @file test_multi_turn_context.cpp
 * @brief Regression test from test2: multi-turn context accumulation.
 *
 * Validates that context grows across turns and that compaction
 * triggers when the threshold is exceeded. Uses mock inference
 * with response queue for multi-turn scripting.
 *
 * @version 1.10.1
 */

#include <entropic/core/engine.h>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mock_inference.h"

using namespace entropic;
using namespace entropic::test;

namespace {

/**
 * @brief Build messages with system prompt and one user turn.
 * @param user_text User message content.
 * @return Message vector.
 * @utility
 * @version 1.10.1
 */
std::vector<Message> make_messages(const std::string& user_text) {
    Message sys;
    sys.role = "system";
    sys.content = "You are a helpful assistant.";
    Message usr;
    usr.role = "user";
    usr.content = user_text;
    return {sys, usr};
}

/**
 * @brief Append a user turn to an existing conversation.
 * @param messages Existing message list.
 * @param text User message content.
 * @utility
 * @version 1.10.1
 */
void add_user_turn(std::vector<Message>& messages,
                   const std::string& text) {
    Message usr;
    usr.role = "user";
    usr.content = text;
    messages.push_back(usr);
}

/**
 * @brief Count messages with a specific role.
 * @param messages Message list.
 * @param role Role to count.
 * @return Count of matching messages.
 * @utility
 * @version 1.10.1
 */
int count_role(const std::vector<Message>& messages,
               const std::string& role) {
    int n = 0;
    for (const auto& m : messages) {
        if (m.role == role) { ++n; }
    }
    return n;
}

} // anonymous namespace

SCENARIO("Context accumulates across turns",
         "[regression][test2]")
{
    GIVEN("an engine with scripted multi-turn responses") {
        MockInference mock;
        mock.response = "Response to turn.";
        auto iface = make_mock_interface(mock);

        LoopConfig lc;
        lc.max_iterations = 1;
        CompactionConfig cc;
        cc.enabled = false;
        AgentEngine engine(iface, lc, cc);

        WHEN("three turns are executed sequentially") {
            auto msgs = make_messages("Turn 1");
            auto r1 = engine.run(msgs);

            add_user_turn(r1, "Turn 2");
            engine.reset_interrupt();
            auto r2 = engine.run(r1);

            add_user_turn(r2, "Turn 3");
            engine.reset_interrupt();
            auto r3 = engine.run(r2);

            THEN("context grows with each turn") {
                REQUIRE(count_role(r1, "assistant") == 1);
                REQUIRE(count_role(r2, "assistant") == 2);
                REQUIRE(count_role(r3, "assistant") == 3);
            }

            THEN("all user messages are preserved") {
                REQUIRE(count_role(r3, "user") == 3);
            }
        }
    }
}

SCENARIO("Compaction triggers at threshold",
         "[regression][test2]")
{
    GIVEN("an engine with a very low compaction threshold") {
        MockInference mock;
        // ~2000 chars = ~500 tokens. With 16384 max and 0.05
        // threshold, 819 tokens triggers compaction.
        mock.response = std::string(8000, 'A');
        auto iface = make_mock_interface(mock);

        LoopConfig lc;
        lc.max_iterations = 1;
        CompactionConfig cc;
        cc.enabled = true;
        cc.threshold_percent = 0.05f;
        AgentEngine engine(iface, lc, cc);

        EngineCallbacks cb{};
        bool compacted = false;
        cb.on_compaction = [](const char*, void* ud) {
            *static_cast<bool*>(ud) = true;
        };
        cb.user_data = &compacted;
        engine.set_callbacks(cb);

        WHEN("context exceeds the threshold") {
            auto msgs = make_messages("Long prompt " +
                                      std::string(8000, 'x'));
            engine.run(msgs);

            THEN("compaction callback fires") {
                REQUIRE(compacted);
            }
        }
    }
}
