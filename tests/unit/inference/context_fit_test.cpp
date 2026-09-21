// SPDX-License-Identifier: Apache-2.0
/**
 * @file context_fit_test.cpp
 * @brief v2.13.0: a prompt that cannot fit the tier context is REFUSED.
 *
 * The numbers in the pure-arithmetic scenarios are the ones the v2.13.0
 * release gate actually logged (build/test-reports/model/logs/
 * test-gh158-concurrent.log): a handle staged 27 tools (18,594 bytes) into a
 * tier configured `context_length: 2048`, rendered a 5,026-token prompt, and
 * llama.cpp answered EVERY turn with
 *
 *     Decode chunk failed (slot=0, start=287, off=1536, chunk=512)
 *     Cache restore failed, falling back to full prefill
 *
 * until the engine gave up with "empty-turn allowance exhausted (n=3/3)".
 * Forty assertions in that test passed while this happened, because nothing
 * in the stack ever said "this prompt does not fit".
 *
 * The backend scenario is the one that pins the behaviour: it drives a real
 * LlamaCppBackend through the real generate path with the v2.3.10 Tokenizer
 * and Sampler seams standing in for the model, and asserts the typed error
 * AND that ZERO tokens were decoded. On unfixed code it reaches the prefill
 * and reports ENTROPIC_ERROR_GENERATE_FAILED ("Prefill decode failed") —
 * the generic failure that made a structural misconfiguration look like a
 * flaky model.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/inference/sampler.h>
#include <entropic/inference/tokenizer.h>
#include <entropic/types/config.h>
#include <entropic/types/error.h>
#include <entropic/types/message.h>
#include "../../../src/inference/context_fit.h"
#include "../../../src/inference/llama_cpp_backend.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

/// @brief The staged-tool overflow exactly as the release gate logged it.
/// @return ContextFit carrying the observed numbers.
/// @internal
/// @version 2.13.0
entropic::ContextFit gate_observation() {
    entropic::ContextFit f;
    f.prompt_tokens = 5026;
    f.context_length = 2048;
    f.system_tokens = 180;
    f.tool_tokens = 4648;
    f.tool_bytes = 18594;
    f.tool_count = 27;
    return f;
}

/// @brief True when `hay` contains `needle`. @internal @version 2.13.0
bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

/**
 * @brief Tokenizer whose token count tracks text length.
 *
 * Four characters per token, the same ratio the engine's own estimator
 * uses. Length-proportional rather than a fixed canned vector, so the
 * system prompt, the staged tool block and the rendered prompt each get a
 * DIFFERENT count and the "largest contributor" attribution is exercised
 * rather than accidentally satisfied.
 *
 * @internal
 * @version 2.13.0
 */
class ProportionalTokenizer : public entropic::Tokenizer {
public:
    std::vector<int32_t> tokenize(const std::string& text,
                                  bool /*add_special*/) const override {
        return std::vector<int32_t>(text.size() / 4 + 1, 1);
    }
    std::string detokenize(int32_t /*token*/) const override { return "x"; }
};

/// @brief Sampler that never runs — a refusal must not reach it.
/// @internal @version 2.13.0
class CountingSampler : public entropic::Sampler {
public:
    int* samples;
    explicit CountingSampler(int* c) : samples(c) {}
    int32_t sample() override { ++(*samples); return 0; }
};

/// @brief Factory vending CountingSampler. @internal @version 2.13.0
class CountingSamplerFactory : public entropic::SamplerFactory {
public:
    int samples = 0;
    std::unique_ptr<entropic::Sampler> create(
        const entropic::GenerationParams& /*params*/) override {
        return std::make_unique<CountingSampler>(&samples);
    }
};

/**
 * @brief LlamaCppBackend with the llama.cpp load/activate steps stubbed.
 *
 * Everything else — render, tokenize, the admission gate, the prefill call
 * site — is the production code path. The point is to exercise
 * `do_generate` for real on a CPU runner with no GGUF.
 *
 * @internal
 * @version 2.13.0
 */
class HeadlessBackend : public entropic::LlamaCppBackend {
protected:
    bool do_load(const entropic::ModelConfig& /*cfg*/) override {
        return true;
    }
    bool do_activate() override { return true; }
};

/// @brief An MCP tool-list array of roughly `bytes` bytes.
/// @param bytes Target payload size.
/// @return JSON array string with enough entries to reach it.
/// @internal
/// @version 2.13.0
std::string big_tool_menu(std::size_t bytes) {
    std::string out = "[";
    int n = 0;
    while (out.size() < bytes) {
        if (n > 0) { out += ","; }
        out += R"({"name":"filesystem.tool_)" + std::to_string(n)
             + R"(","description":"A tool that does a thing, described at )"
               R"(the length a real MCP server describes its tools, which )"
               R"(is where the prompt budget actually goes.",)"
               R"("inputSchema":{"type":"object","properties":{"path":)"
               R"({"type":"string"},"content":{"type":"string"}},)"
               R"("required":["path"]}})";
        ++n;
    }
    out += "]";
    return out;
}

}  // namespace

// ── Pure arithmetic: the decision ───────────────────────────

SCENARIO("context_fit_overflows refuses only what genuinely cannot fit",
         "[v2.13.0][inference][context_fit]")
{
    GIVEN("the tier context the release gate ran with") {
        auto f = gate_observation();

        WHEN("the prompt is the 5026 tokens that gate rendered") {
            THEN("the turn is refused") {
                REQUIRE(entropic::context_fit_overflows(f));
            }
        }

        WHEN("the prompt is one token short of the window") {
            f.prompt_tokens = f.context_length - 1;
            THEN("it is admitted — there is room to emit") {
                REQUIRE_FALSE(entropic::context_fit_overflows(f));
            }
        }

        WHEN("the prompt exactly fills the window") {
            f.prompt_tokens = f.context_length;
            THEN("it is refused — zero positions left to decode into") {
                REQUIRE(entropic::context_fit_overflows(f));
            }
        }

        WHEN("context_length is unset") {
            f.context_length = 0;
            THEN("nothing is refused — the budget is unknown, not zero") {
                REQUIRE_FALSE(entropic::context_fit_overflows(f));
            }
        }
    }
}

// ── Pure arithmetic: the diagnosis ──────────────────────────

SCENARIO("the refusal names every number an operator needs",
         "[v2.13.0][inference][context_fit]")
{
    GIVEN("the release gate's overflow") {
        const auto f = gate_observation();

        WHEN("the message is built") {
            const std::string msg = entropic::context_overflow_message(f);
            INFO("message: " << msg);

            THEN("it carries the count, the budget and the overshoot") {
                CHECK(has(msg, "5026"));
                CHECK(has(msg, "context_length=2048"));
                CHECK(has(msg, "2978"));
            }
            AND_THEN("it names the dominant contributor") {
                CHECK(has(msg, "Largest contributor: the staged tool block"));
            }
            AND_THEN("it sizes the tool block in tools AND bytes") {
                CHECK(has(msg, "27 tools"));
                CHECK(has(msg, "18594 bytes"));
            }
            AND_THEN("it names the knobs, and promises no silent fix") {
                CHECK(has(msg, "context_length"));
                CHECK(has(msg, "allowed_tools"));
                CHECK(has(msg, "Refused before decode"));
            }
        }
    }
}

SCENARIO("the largest contributor is attributed to the right part",
         "[v2.13.0][inference][context_fit]")
{
    GIVEN("an overflow dominated by the system prompt") {
        entropic::ContextFit f;
        f.prompt_tokens = 9000;
        f.context_length = 4096;
        f.system_tokens = 8000;
        f.tool_tokens = 500;
        THEN("the system prompt is named") {
            CHECK(entropic::context_fit_largest_contributor(f)
                  == "the system prompt");
        }
    }

    GIVEN("an overflow dominated by accumulated history") {
        entropic::ContextFit f;
        f.prompt_tokens = 9000;
        f.context_length = 4096;
        f.system_tokens = 200;
        f.tool_tokens = 800;
        THEN("the history is named, and sized by subtraction") {
            CHECK(entropic::context_fit_largest_contributor(f)
                  == "the message history");
            CHECK(entropic::context_fit_history_tokens(f) == 8000);
        }
    }

    GIVEN("attributions that over-count the prompt") {
        entropic::ContextFit f;
        f.prompt_tokens = 100;
        f.context_length = 64;
        f.system_tokens = 90;
        f.tool_tokens = 90;
        THEN("the history remainder floors at zero, never negative") {
            CHECK(entropic::context_fit_history_tokens(f) == 0);
        }
    }
}

// ── The backend refuses, and does not decode ────────────────

SCENARIO("a tier whose staged tools overflow its context refuses the turn "
         "before any decode",
         "[v2.13.0][inference][context_fit][backend]")
{
    GIVEN("a 2048-token tier with a 27-tool menu staged on it") {
        HeadlessBackend backend;

        entropic::ModelConfig cfg;
        cfg.path = "/nonexistent/headless.gguf";
        cfg.adapter = "gemma4";
        cfg.context_length = 2048;
        REQUIRE(backend.load(cfg));
        backend.inject_tokenizer_for_test(
            std::make_unique<ProportionalTokenizer>());
        auto factory = std::make_unique<CountingSamplerFactory>();
        auto* factory_raw = factory.get();
        backend.inject_sampler_factory_for_test(std::move(factory));
        REQUIRE(backend.activate());

        // 18,594 bytes is what the gate logged: "Active tools staged for
        // common_chat render: 18594 bytes".
        const std::string menu = big_tool_menu(18594);
        backend.set_active_tools(menu);

        // A conversation that has already accumulated past the window. The
        // engine's compaction ran before this point; whatever is left here
        // is the irreducible prompt.
        std::vector<entropic::Message> msgs = {
            {"system", "You are a terse assistant."},
            {"user", std::string(24000, 'a')},
        };
        entropic::GenerationParams params;
        params.max_tokens = 256;

        WHEN("the turn is generated") {
            auto result = backend.generate(msgs, params);
            INFO("error_message: " << result.error_message);
            INFO("input tokens: " << backend.last_input_tokens()
                 << " context_length: " << cfg.context_length);

            THEN("it is refused with the context-full error, not a generic "
                 "generate failure") {
                CHECK(result.error_code
                      == ENTROPIC_ERROR_EVAL_CONTEXT_FULL);
                CHECK(result.finish_reason == "error");
            }
            AND_THEN("the message carries the measured numbers") {
                CHECK(has(result.error_message,
                          std::to_string(backend.last_input_tokens())));
                CHECK(has(result.error_message, "context_length=2048"));
                CHECK(has(result.error_message, "staged tool block"));
            }
            AND_THEN("nothing was decoded and no KV was touched") {
                // The instrumentation half. A refusal that still ran the
                // prefill would satisfy the error assertions above while
                // costing exactly what the guard exists to save.
                CHECK(backend.last_prefill_tokens() == 0);
                CHECK(backend.kv_full_clear_count() == 0);
                CHECK(factory_raw->samples == 0);
            }
        }
    }
}

SCENARIO("a prompt that fits is not refused",
         "[v2.13.0][inference][context_fit][backend]")
{
    GIVEN("the same headless tier with a short prompt and no tools") {
        HeadlessBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = "/nonexistent/headless.gguf";
        cfg.context_length = 2048;
        REQUIRE(backend.load(cfg));
        backend.inject_tokenizer_for_test(
            std::make_unique<ProportionalTokenizer>());
        backend.inject_sampler_factory_for_test(
            std::make_unique<CountingSamplerFactory>());
        REQUIRE(backend.activate());

        std::vector<entropic::Message> msgs = {
            {"system", "You are terse."},
            {"user", "Name one colour."},
        };
        entropic::GenerationParams params;
        params.max_tokens = 8;

        WHEN("the turn is generated") {
            auto result = backend.generate(msgs, params);
            INFO("error_message: " << result.error_message);

            THEN("the admission gate stays out of the way") {
                // It fails later, in the prefill, because there is no
                // llama_context — which is the POINT: the guard did not
                // fire, so the generic decode failure is what surfaces.
                CHECK(result.error_code != ENTROPIC_ERROR_EVAL_CONTEXT_FULL);
                CHECK(backend.last_input_tokens() < cfg.context_length);
            }
        }
    }
}
