// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file response_generator_test.cpp
 * @brief BDD tests for ResponseGenerator.
 * @version 1.10.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/core/response_generator.h>
#include <entropic/core/engine_types.h>

#include <atomic>
#include <cstring>
#include <string>

using namespace entropic;

// ── Mock inference helpers ───────────────────────────────

/**
 * @brief Null-op generate returning canned content.
 * @version 1.10.0
 * @internal
 */
static int mock_generate(
    const char* /*msgs*/, const char* /*params*/,
    char** result_json, void* /*data*/)
{
    *result_json = strdup("Hello from mock");
    return 0;
}

/**
 * @brief Mock free for strdup'd results.
 * @version 1.10.0
 * @internal
 */
static void mock_free(void* ptr)
{
    free(ptr);
}

/**
 * @brief Mock route that returns "eng" tier.
 * @version 1.10.0
 * @internal
 */
static int mock_route(
    const char* /*msgs*/, char** result, void* /*data*/)
{
    *result = strdup("eng");
    return 0;
}

/**
 * @brief Mock is_response_complete that returns true for non-empty content.
 * @version 1.10.0
 * @internal
 */
static int mock_is_complete(
    const char* content, const char* /*tool_calls*/, void* /*data*/)
{
    return (content != nullptr && content[0] != '\0') ? 1 : 0;
}

/**
 * @brief Build an InferenceInterface with mock functions.
 * @return Populated interface struct.
 * @version 1.10.0
 * @internal
 */
static InferenceInterface make_mock_inference()
{
    InferenceInterface iface{};
    iface.generate = mock_generate;
    iface.free_fn = mock_free;
    iface.route = mock_route;
    iface.is_response_complete = mock_is_complete;
    return iface;
}

/**
 * @brief Build a minimal LoopConfig.
 * @return Default loop config.
 * @version 1.10.0
 * @internal
 */
static LoopConfig make_loop_config()
{
    LoopConfig cfg{};
    cfg.max_iterations = 15;
    cfg.max_consecutive_errors = 3;
    cfg.stream_output = false;
    return cfg;
}

// ── Tests ────────────────────────────────────────────────

SCENARIO("ResponseGenerator batch generation", "[core][response_generator]") {
    GIVEN("a ResponseGenerator with mock inference") {
        auto iface = make_mock_inference();
        auto loop_cfg = make_loop_config();
        EngineCallbacks callbacks{};
        GenerationEvents events{};

        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        LoopContext ctx{};
        ctx.state = AgentState::EXECUTING;
        Message msg;
        msg.role = "user";
        msg.content = "Hello";
        ctx.messages.push_back(std::move(msg));

        WHEN("generate_response is called") {
            auto result = gen.generate_response(ctx);
            THEN("it returns content from the mock") {
                REQUIRE(result.content == "Hello from mock");
                REQUIRE(result.finish_reason == "stop");
            }
        }
    }
}

SCENARIO("ResponseGenerator locks tier via routing",
         "[core][response_generator]") {
    GIVEN("a context with no locked tier") {
        auto iface = make_mock_inference();
        auto loop_cfg = make_loop_config();
        EngineCallbacks callbacks{};
        GenerationEvents events{};

        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        LoopContext ctx{};
        ctx.state = AgentState::EXECUTING;
        Message msg;
        msg.role = "user";
        msg.content = "test";
        ctx.messages.push_back(std::move(msg));

        WHEN("generate_response is called") {
            gen.generate_response(ctx);
            THEN("tier is locked to router result") {
                REQUIRE(ctx.locked_tier == "eng");
            }
        }
    }

    GIVEN("a context with a pre-locked tier") {
        auto iface = make_mock_inference();
        auto loop_cfg = make_loop_config();
        EngineCallbacks callbacks{};
        GenerationEvents events{};

        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        LoopContext ctx{};
        ctx.state = AgentState::EXECUTING;
        ctx.locked_tier = "arch";
        Message msg;
        msg.role = "user";
        msg.content = "test";
        ctx.messages.push_back(std::move(msg));

        WHEN("generate_response is called") {
            gen.generate_response(ctx);
            THEN("tier remains unchanged") {
                REQUIRE(ctx.locked_tier == "arch");
            }
        }
    }
}

SCENARIO("ResponseGenerator defaults tier when no router",
         "[core][response_generator]") {
    GIVEN("an inference interface with no route function") {
        auto iface = make_mock_inference();
        iface.route = nullptr;
        auto loop_cfg = make_loop_config();
        EngineCallbacks callbacks{};
        GenerationEvents events{};

        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        LoopContext ctx{};
        ctx.state = AgentState::EXECUTING;
        Message msg;
        msg.role = "user";
        msg.content = "test";
        ctx.messages.push_back(std::move(msg));

        WHEN("generate_response is called") {
            gen.generate_response(ctx);
            THEN("tier defaults to 'default'") {
                REQUIRE(ctx.locked_tier == "default");
            }
        }
    }
}

SCENARIO("is_response_complete delegates to inference interface",
         "[core][response_generator]") {
    GIVEN("a ResponseGenerator with mock is_complete") {
        auto iface = make_mock_inference();
        auto loop_cfg = make_loop_config();
        EngineCallbacks callbacks{};
        GenerationEvents events{};

        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        WHEN("called with non-empty content") {
            bool complete = gen.is_response_complete("Hello", "[]");
            THEN("it returns true") {
                REQUIRE(complete);
            }
        }

        WHEN("called with empty content") {
            bool complete = gen.is_response_complete("", "[]");
            THEN("it returns false") {
                REQUIRE_FALSE(complete);
            }
        }
    }

    GIVEN("a ResponseGenerator with no is_complete callback") {
        auto iface = make_mock_inference();
        iface.is_response_complete = nullptr;
        auto loop_cfg = make_loop_config();
        EngineCallbacks callbacks{};
        GenerationEvents events{};

        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        WHEN("called with non-empty content") {
            bool complete = gen.is_response_complete("Hello", "[]");
            THEN("it falls back to non-empty check") {
                REQUIRE(complete);
            }
        }
    }
}

SCENARIO("generate_batch returns error on null generate function",
         "[core][response_generator]") {
    GIVEN("an inference interface with no generate function") {
        auto iface = make_mock_inference();
        iface.generate = nullptr;
        auto loop_cfg = make_loop_config();
        EngineCallbacks callbacks{};
        GenerationEvents events{};

        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        LoopContext ctx{};
        ctx.state = AgentState::EXECUTING;
        ctx.locked_tier = "default";
        Message msg;
        msg.role = "user";
        msg.content = "test";
        ctx.messages.push_back(std::move(msg));

        WHEN("generate_response is called") {
            auto result = gen.generate_response(ctx);
            THEN("finish_reason is error") {
                REQUIRE(result.finish_reason == "error");
            }
        }
    }
}
