// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file json_serializers_test.cpp
 * @brief Unit tests for facade-private JSON serialization helpers
 *        (``src/facade/json_serializers.h``).
 *
 * Boosts librentropic-facade coverage by exercising every inline
 * function in the private header. ``serialize_messages_utf8_test.cpp``
 * already pins the malformed-UTF-8 contract for ``serialize_messages``;
 * this file covers the remaining surface — empty/multi-element lists,
 * ``parse`` edge cases (null/malformed/empty/single/multi/missing-fields),
 * and the ``serialize_adapter_info`` / ``serialize_adapter_list``
 * round-trips that have no other test coverage today.
 *
 * @version 2.3.8
 */

#include "json_serializers.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <entropic/types/config.h>
#include <entropic/types/content.h>
#include <entropic/types/message.h>
#include <entropic/types/messages_json.h>

#include <string>
#include <vector>

using entropic::Message;
using entropic::AdapterInfo;
using entropic::AdapterState;

namespace {

/**
 * @brief Build a Message with the (pointer, size) string constructor so
 *        embedded NULs / malformed bytes are preserved verbatim.
 * @internal
 * @version 2.3.8
 */
Message make_msg(const std::string& role, const std::string& content) {
    Message m;
    m.role = role;
    m.content = content;
    return m;
}

} // namespace

// ── serialize_messages ────────────────────────────────────────────────

SCENARIO("serialize_messages handles an empty message list",
         "[json_serializers][facade]") {
    GIVEN("an empty message vector") {
        std::vector<Message> messages;
        WHEN("serialize_messages is called") {
            auto out = facade_json::serialize_messages(messages);
            THEN("it returns the empty JSON array literal") {
                REQUIRE(out == "[]");
            }
        }
    }
}

SCENARIO("serialize_messages emits role + content for a single message",
         "[json_serializers][facade]") {
    GIVEN("a vector with one assistant message") {
        std::vector<Message> messages{make_msg("assistant", "Hello world")};
        WHEN("serialize_messages is called") {
            auto out = facade_json::serialize_messages(messages);
            THEN("it round-trips through nlohmann::json::parse") {
                auto parsed = nlohmann::json::parse(out);
                REQUIRE(parsed.is_array());
                REQUIRE(parsed.size() == 1);
                REQUIRE(parsed[0].at("role").get<std::string>() == "assistant");
                REQUIRE(parsed[0].at("content").get<std::string>()
                        == "Hello world");
            }
        }
    }
}

SCENARIO("serialize_messages preserves order across multiple messages",
         "[json_serializers][facade]") {
    GIVEN("user → assistant → tool with valid UTF-8 content") {
        std::vector<Message> messages{
            make_msg("user", "Hi"),
            make_msg("assistant", "Hello!"),
            make_msg("tool", "{\"result\":42}"),
        };
        WHEN("serialize_messages is called") {
            auto out = facade_json::serialize_messages(messages);
            THEN("the array preserves role order and content") {
                auto parsed = nlohmann::json::parse(out);
                REQUIRE(parsed.size() == 3);
                REQUIRE(parsed[0].at("role").get<std::string>() == "user");
                REQUIRE(parsed[1].at("role").get<std::string>() == "assistant");
                REQUIRE(parsed[2].at("role").get<std::string>() == "tool");
                REQUIRE(parsed[2].at("content").get<std::string>()
                        == "{\"result\":42}");
            }
        }
    }
}

// ── parse ──────────────────────────────────────────────────────────────

SCENARIO("facade_json::parse returns a null JSON value for a null pointer",
         "[json_serializers][facade]") {
    GIVEN("a null C string") {
        WHEN("parse is called") {
            auto j = facade_json::parse(nullptr);
            THEN("the result is a null JSON value") {
                REQUIRE(j.is_null());
            }
        }
    }
}

SCENARIO("facade_json::parse returns discarded value on malformed JSON",
         "[json_serializers][facade]") {
    GIVEN("a string that is not valid JSON") {
        const char* bad = "{not: valid json,,,}";
        WHEN("parse is called") {
            auto j = facade_json::parse(bad);
            THEN("the result is discarded — neither object nor array") {
                // nlohmann::json with allow_exceptions=false returns a
                // value_t::discarded sentinel which is_discarded()==true.
                REQUIRE(j.is_discarded());
            }
        }
    }
}

SCENARIO("facade_json::parse handles the empty array literal",
         "[json_serializers][facade]") {
    GIVEN("the empty array literal '[]'") {
        WHEN("parse is called") {
            auto j = facade_json::parse("[]");
            THEN("the result is an empty array") {
                REQUIRE(j.is_array());
                REQUIRE(j.empty());
            }
        }
    }
}

SCENARIO("facade_json::parse round-trips a single message",
         "[json_serializers][facade]") {
    GIVEN("one message JSON") {
        const char* one = R"([{"role":"user","content":"hi"}])";
        WHEN("parse is called") {
            auto j = facade_json::parse(one);
            THEN("the array contains the parsed message") {
                REQUIRE(j.is_array());
                REQUIRE(j.size() == 1);
                REQUIRE(j[0].at("role").get<std::string>() == "user");
                REQUIRE(j[0].at("content").get<std::string>() == "hi");
            }
        }
    }
}

SCENARIO("facade_json::parse round-trips multiple messages",
         "[json_serializers][facade]") {
    GIVEN("multi-message JSON") {
        const char* many = R"([
            {"role":"system","content":"sp"},
            {"role":"user","content":"hi"},
            {"role":"assistant","content":"hello"}
        ])";
        WHEN("parse is called") {
            auto j = facade_json::parse(many);
            THEN("each entry is preserved") {
                REQUIRE(j.size() == 3);
                REQUIRE(j[0].at("role").get<std::string>() == "system");
                REQUIRE(j[1].at("role").get<std::string>() == "user");
                REQUIRE(j[2].at("role").get<std::string>() == "assistant");
            }
        }
    }
}

SCENARIO("facade_json::parse leaves missing fields absent (no auto-fill)",
         "[json_serializers][facade]") {
    GIVEN("a message missing the 'content' field") {
        const char* missing = R"([{"role":"user"}])";
        WHEN("parse is called") {
            auto j = facade_json::parse(missing);
            THEN("the parsed object exposes the absence") {
                REQUIRE(j.size() == 1);
                REQUIRE(j[0].contains("role"));
                REQUIRE_FALSE(j[0].contains("content"));
            }
        }
    }
}

// ── obj / arr factory helpers ──────────────────────────────────────────

SCENARIO("facade_json::obj returns an empty JSON object",
         "[json_serializers][facade]") {
    GIVEN("a fresh call to facade_json::obj()") {
        WHEN("the value is inspected") {
            auto j = facade_json::obj();
            THEN("it is an empty object") {
                REQUIRE(j.is_object());
                REQUIRE(j.empty());
            }
        }
    }
}

SCENARIO("facade_json::arr returns an empty JSON array",
         "[json_serializers][facade]") {
    GIVEN("a fresh call to facade_json::arr()") {
        WHEN("the value is inspected") {
            auto j = facade_json::arr();
            THEN("it is an empty array") {
                REQUIRE(j.is_array());
                REQUIRE(j.empty());
            }
        }
    }
}

// ── serialize_adapter_info ─────────────────────────────────────────────

SCENARIO("serialize_adapter_info emits all AdapterInfo fields",
         "[json_serializers][facade]") {
    GIVEN("a populated AdapterInfo") {
        AdapterInfo ai;
        ai.name = "qwen-lora-a";
        ai.path = "/models/adapters/qwen-lora-a.gguf";
        ai.state = AdapterState::WARM;
        ai.scale = 0.75f;
        ai.tier_name = "lead";
        ai.base_model_path = "/models/qwen.gguf";
        ai.ram_bytes = 1024ull * 1024ull * 64ull;

        WHEN("serialize_adapter_info is called") {
            auto out = facade_json::serialize_adapter_info(ai);
            THEN("every field is present in the JSON object") {
                auto j = nlohmann::json::parse(out);
                REQUIRE(j.at("name").get<std::string>() == "qwen-lora-a");
                REQUIRE(j.at("path").get<std::string>()
                        == "/models/adapters/qwen-lora-a.gguf");
                REQUIRE(j.at("state").get<int>()
                        == static_cast<int>(AdapterState::WARM));
                REQUIRE(j.at("scale").get<float>() == 0.75f);
                REQUIRE(j.at("tier_name").get<std::string>() == "lead");
                REQUIRE(j.at("base_model_path").get<std::string>()
                        == "/models/qwen.gguf");
                REQUIRE(j.at("ram_bytes").get<size_t>()
                        == 1024ull * 1024ull * 64ull);
            }
        }
    }
}

SCENARIO("serialize_adapter_info handles a default-constructed AdapterInfo",
         "[json_serializers][facade]") {
    GIVEN("a default-constructed AdapterInfo (state=COLD, scale=1.0)") {
        AdapterInfo ai;
        WHEN("serialize_adapter_info is called") {
            auto out = facade_json::serialize_adapter_info(ai);
            THEN("defaults appear in the JSON object") {
                auto j = nlohmann::json::parse(out);
                REQUIRE(j.at("name").get<std::string>().empty());
                REQUIRE(j.at("state").get<int>()
                        == static_cast<int>(AdapterState::COLD));
                REQUIRE(j.at("scale").get<float>() == 1.0f);
                REQUIRE(j.at("ram_bytes").get<size_t>() == 0);
            }
        }
    }
}

// ── serialize_adapter_list ─────────────────────────────────────────────

SCENARIO("serialize_adapter_list emits an empty array for no adapters",
         "[json_serializers][facade]") {
    GIVEN("an empty adapter list") {
        std::vector<AdapterInfo> empty;
        WHEN("serialize_adapter_list is called") {
            auto out = facade_json::serialize_adapter_list(empty);
            THEN("the output is exactly '[]'") {
                REQUIRE(out == "[]");
            }
        }
    }
}

SCENARIO("serialize_adapter_list summarizes each adapter into the array",
         "[json_serializers][facade]") {
    GIVEN("two adapters with distinct states") {
        AdapterInfo a, b;
        a.name = "one";
        a.state = AdapterState::WARM;
        a.scale = 1.0f;
        a.tier_name = "lead";
        b.name = "two";
        b.state = AdapterState::COLD;
        b.scale = 0.5f;
        b.tier_name = "support";
        std::vector<AdapterInfo> list{a, b};

        WHEN("serialize_adapter_list is called") {
            auto out = facade_json::serialize_adapter_list(list);
            THEN("the JSON array carries the summary fields") {
                auto j = nlohmann::json::parse(out);
                REQUIRE(j.is_array());
                REQUIRE(j.size() == 2);
                REQUIRE(j[0].at("name").get<std::string>() == "one");
                REQUIRE(j[0].at("state").get<int>()
                        == static_cast<int>(AdapterState::WARM));
                REQUIRE(j[0].at("tier_name").get<std::string>() == "lead");
                REQUIRE(j[1].at("name").get<std::string>() == "two");
                REQUIRE(j[1].at("scale").get<float>() == 0.5f);
            }
        }
    }
}

// ── gh#165: serialize_messages must be LOSSLESS (v2.13.0) ─────────────

/**
 * @brief A message carrying every field a restore has to preserve.
 * @return Message with metadata AND multimodal content_parts.
 * @internal
 * @version 2.13.0
 */
static Message rich_message() {
    Message m;
    m.role = "tool";
    m.content = "describe this";
    m.metadata["tool_name"] = "filesystem.read";
    m.metadata["is_context_anchor"] = "true";
    entropic::ContentPart text;
    text.type = entropic::ContentPartType::TEXT;
    text.text = "describe this";
    entropic::ContentPart image;
    image.type = entropic::ContentPartType::IMAGE;
    image.image_path = "/tmp/foo.png";
    image.image_url = "https://example.invalid/foo.png";
    image.width = 512;
    image.height = 384;
    m.content_parts = {text, image};
    return m;
}

SCENARIO("gh#165: get -> set -> get is byte-identical, metadata included",
         "[json_serializers][facade][gh165][2.13.0]") {
    GIVEN("a conversation carrying metadata and content_parts") {
        // gh#144 gave consumers entropic_session_context_get but no write
        // counterpart, so a session could not survive a restart. The write
        // is only worth having if the read is lossless: serialize_messages
        // emitted ONLY role + content, so a restored session silently lost
        // every tool_name (context_manager, compaction and the engine's
        // tool-result folding all key off it) and every image part.
        std::vector<Message> original{
            make_msg("system", "You are terse."),
            rich_message(),
        };

        WHEN("the conversation is serialized, parsed back and re-serialized") {
            const std::string first =
                facade_json::serialize_messages(original);
            const auto restored = entropic::parse_messages_json(first.c_str());
            const std::string second =
                facade_json::serialize_messages(restored);

            THEN("the two serializations are byte-identical") {
                // The whole contract in one assertion: whatever a consumer
                // stores, feeding it back reproduces it exactly.
                CHECK(second == first);
            }
            AND_THEN("metadata survived the round trip") {
                REQUIRE(restored.size() == 2);
                CHECK(restored[1].metadata.at("tool_name")
                      == "filesystem.read");
                CHECK(restored[1].metadata.at("is_context_anchor") == "true");
            }
            AND_THEN("content_parts survived, including image geometry") {
                REQUIRE(restored[1].content_parts.size() == 2);
                CHECK(restored[1].content_parts[0].type
                      == entropic::ContentPartType::TEXT);
                CHECK(restored[1].content_parts[0].text == "describe this");
                const auto& img = restored[1].content_parts[1];
                CHECK(img.type == entropic::ContentPartType::IMAGE);
                CHECK(img.image_path == "/tmp/foo.png");
                CHECK(img.image_url == "https://example.invalid/foo.png");
                CHECK(img.width == 512);
                CHECK(img.height == 384);
            }
        }
    }
}

SCENARIO("gh#165: the addition is additive — plain messages are unchanged",
         "[json_serializers][facade][gh165][2.13.0]") {
    GIVEN("a message with no metadata and no content parts") {
        std::vector<Message> plain{make_msg("user", "Hi")};

        WHEN("it is serialized") {
            const auto out = facade_json::serialize_messages(plain);

            THEN("the emitted object is exactly role + content") {
                // A consumer reading role/content must see no new keys for
                // the messages it has always seen — the new fields appear
                // only when there is something to carry.
                CHECK(out == R"([{"content":"Hi","role":"user"}])");
            }
        }
    }
}
