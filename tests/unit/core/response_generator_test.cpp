// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file response_generator_test.cpp
 * @brief BDD tests for ResponseGenerator.
 * @version 2.0.6-rc16
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

SCENARIO("stream observer fires for batch generation path",
         "[core][response_generator][observer]") {
    GIVEN("a ResponseGenerator with a registered observer") {
        auto iface = make_mock_inference();
        auto loop_cfg = make_loop_config();  // stream_output=false
        EngineCallbacks callbacks{};
        GenerationEvents events{};
        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        struct Sink {
            std::string buf;
            int calls = 0;
        } sink;
        gen.set_stream_observer(
            [](const char* t, size_t n, void* ud) {
                auto* s = static_cast<Sink*>(ud);
                s->buf.append(t, n);
                s->calls++;
            }, &sink);

        LoopContext ctx{};
        ctx.state = AgentState::EXECUTING;
        ctx.locked_tier = "default";
        Message msg;
        msg.role = "user";
        msg.content = "ping";
        ctx.messages.push_back(std::move(msg));

        WHEN("generate_response runs the batch fallback") {
            auto result = gen.generate_response(ctx);
            THEN("observer receives the full content exactly once") {
                REQUIRE(result.content == "Hello from mock");
                REQUIRE(sink.buf == "Hello from mock");
                REQUIRE(sink.calls == 1);
            }
        }
    }
}

SCENARIO("stream observer getters reflect registration",
         "[core][response_generator][observer]") {
    GIVEN("a freshly constructed ResponseGenerator") {
        auto iface = make_mock_inference();
        auto loop_cfg = make_loop_config();
        EngineCallbacks callbacks{};
        GenerationEvents events{};
        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        THEN("observer is unset by default") {
            REQUIRE(gen.stream_observer() == nullptr);
            REQUIRE(gen.stream_observer_data() == nullptr);
        }

        WHEN("an observer is registered and then cleared") {
            int sentinel = 0;
            auto cb = [](const char*, size_t, void*) {};
            gen.set_stream_observer(cb, &sentinel);
            REQUIRE(gen.stream_observer() == cb);
            REQUIRE(gen.stream_observer_data() == &sentinel);

            gen.set_stream_observer(nullptr, nullptr);
            THEN("getters report the cleared state") {
                REQUIRE(gen.stream_observer() == nullptr);
                REQUIRE(gen.stream_observer_data() == nullptr);
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

// ── P3-19: Partial-result preservation across mid-stream crash ────

namespace {

/**
 * @brief Shared control block for streaming mock behaviour.
 * @internal
 * @version 2.0.6-rc16
 */
struct StreamControl {
    std::string emit_content;
    int return_code = 0;
};

/**
 * @brief Mock streaming function that emits content then returns rc.
 * @version 2.0.6-rc16
 * @internal
 */
static int mock_generate_stream_impl(
    const char* /*msgs*/,
    const char* /*params*/,
    void (*on_token)(const char*, size_t, void*),
    void* cb_data,
    int* /*cancel*/,
    void* user_data)
{
    auto* ctrl = static_cast<StreamControl*>(user_data);
    if (on_token && !ctrl->emit_content.empty()) {
        on_token(ctrl->emit_content.c_str(),
                 ctrl->emit_content.size(), cb_data);
    }
    return ctrl->return_code;
}

} // namespace

SCENARIO("generate_streaming preserves partial content on non-zero rc",
         "[core][response_generator][P3-19][2.0.6-rc16]")
{
    GIVEN("a ResponseGenerator backed by a streaming mock") {
        StreamControl ctrl;
        auto iface = make_mock_inference();
        iface.generate_stream = mock_generate_stream_impl;
        iface.backend_data = &ctrl;
        auto loop_cfg = make_loop_config();
        loop_cfg.stream_output = true;
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

        WHEN("backend emits partial tokens then returns non-zero") {
            ctrl.emit_content = "partial response";
            ctrl.return_code = 1;
            auto result = gen.generate_response(ctx);

            THEN("finish_reason is 'partial' and content is preserved") {
                REQUIRE(result.finish_reason == "partial");
                REQUIRE(result.content == "partial response");
            }
        }

        WHEN("backend returns non-zero with no tokens emitted") {
            ctrl.emit_content = "";
            ctrl.return_code = 1;
            auto result = gen.generate_response(ctx);

            THEN("finish_reason is 'error' and content is empty") {
                REQUIRE(result.finish_reason == "error");
                REQUIRE(result.content.empty());
            }
        }

        WHEN("backend returns 0 (success path)") {
            ctrl.emit_content = "full response";
            ctrl.return_code = 0;
            auto result = gen.generate_response(ctx);

            THEN("finish_reason is 'stop'") {
                REQUIRE(result.finish_reason == "stop");
                REQUIRE(result.content == "full response");
            }
        }
    }
}


// ── Demo ask #1 (v2.1.0): engine state reminder injection ──

/// @brief Module-static capture buffer for the iteration-reminder
/// SCENARIO; mock_generate_capturing writes the JSON-serialized
/// messages here so the test can grep for the reminder line.
static std::string g_captured_msgs;

/**
 * @brief Mock generate that records its msgs argument before returning.
 * @internal
 * @callback
 * @version 2.1.0
 */
static int mock_generate_capturing(
    const char* msgs, const char* /*params*/,
    char** result_json, void* /*data*/)
{
    g_captured_msgs = msgs != nullptr ? std::string(msgs) : std::string();
    *result_json = strdup("ok");
    return 0;
}

SCENARIO("ResponseGenerator injects iteration / budget reminder into system prompt",
         "[core][response_generator][2.1.0][demo-ask-1]")
{
    GIVEN("a ResponseGenerator and a LoopContext at iteration 7/50, 12 tool calls") {
        InferenceInterface iface{};
        iface.generate = mock_generate_capturing;
        iface.free_fn = mock_free;
        iface.is_response_complete = mock_is_complete;
        auto loop_cfg = make_loop_config();
        loop_cfg.max_iterations = 50;
        EngineCallbacks callbacks{};
        GenerationEvents events{};
        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        LoopContext ctx{};
        ctx.state = AgentState::EXECUTING;
        ctx.locked_tier = "lead";
        ctx.metrics.iterations = 7;
        ctx.metrics.tool_calls = 12;
        Message sys{"system", "you are a helpful agent"};
        ctx.messages.push_back(std::move(sys));
        Message user{"user", "do the thing"};
        ctx.messages.push_back(std::move(user));

        g_captured_msgs.clear();

        WHEN("generate_response runs") {
            gen.generate_response(ctx);

            THEN("the captured messages contain the iteration reminder line") {
                CHECK(g_captured_msgs.find("[engine] iteration 7/50")
                      != std::string::npos);
                CHECK(g_captured_msgs.find("tool calls so far: 12")
                      != std::string::npos);
            }
            AND_THEN("the original system content is preserved") {
                CHECK(g_captured_msgs.find("you are a helpful agent")
                      != std::string::npos);
            }
            AND_THEN("ctx.messages itself is not mutated with the reminder") {
                // The reminder is per-turn; persistent ctx.messages must
                // not accumulate iteration tags across turns.
                CHECK(ctx.messages.front().content
                      == "you are a helpful agent");
            }
        }
    }
}

SCENARIO("Engine state reminder honors per-identity max_iterations override",
         "[core][response_generator][2.1.0][demo-ask-1]")
{
    GIVEN("a ResponseGenerator and a context with effective_max_iterations=200") {
        InferenceInterface iface{};
        iface.generate = mock_generate_capturing;
        iface.free_fn = mock_free;
        iface.is_response_complete = mock_is_complete;
        auto loop_cfg = make_loop_config();
        loop_cfg.max_iterations = 15;  // global default
        EngineCallbacks callbacks{};
        GenerationEvents events{};
        ResponseGenerator gen(iface, loop_cfg, callbacks, events);

        LoopContext ctx{};
        ctx.state = AgentState::EXECUTING;
        ctx.locked_tier = "researcher";
        ctx.metrics.iterations = 42;
        ctx.effective_max_iterations = 200;
        Message sys{"system", "researcher prompt"};
        ctx.messages.push_back(std::move(sys));
        Message user{"user", "lookup X"};
        ctx.messages.push_back(std::move(user));

        g_captured_msgs.clear();

        WHEN("generate_response runs") {
            gen.generate_response(ctx);

            THEN("reminder uses the per-identity override, not the global default") {
                CHECK(g_captured_msgs.find("iteration 42/200")
                      != std::string::npos);
                CHECK(g_captured_msgs.find("/15") == std::string::npos);
            }
        }
    }
}
