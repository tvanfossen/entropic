/**
 * @file test_backend_capabilities.cpp
 * @brief Tests for InferenceBackend capability query system (v1.9.13).
 *
 * Uses mock backends to verify capability queries, backend info,
 * state management defaults, and multi-sequence defaults.
 *
 * @version 1.9.13
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/backend.h>

#include <string>

namespace {

/**
 * @brief Mock backend with no capability overrides.
 *
 * Inherits all defaults from InferenceBackend. Used to verify that
 * the base class returns false for all capabilities and provides
 * sensible defaults for all new methods.
 *
 * @version 1.9.13
 */
class DefaultMockBackend : public entropic::InferenceBackend {
protected:
    std::string do_backend_name() const override { return "default_mock"; }

    bool do_load(const entropic::ModelConfig& /*cfg*/) override {
        return true;
    }
    bool do_activate() override { return true; }
    void do_deactivate() override {}
    void do_unload() override {}

    entropic::GenerationResult do_generate(
        const std::vector<entropic::Message>& /*messages*/,
        const entropic::GenerationParams& /*params*/) override
    {
        entropic::GenerationResult r;
        r.content = "default mock";
        r.token_count = 2;
        return r;
    }

    entropic::GenerationResult do_generate_streaming(
        const std::vector<entropic::Message>& /*messages*/,
        const entropic::GenerationParams& /*params*/,
        std::function<void(std::string_view)> /*on_token*/,
        std::atomic<bool>& /*cancel*/) override
    {
        return {};
    }

    entropic::GenerationResult do_complete(
        const std::string& /*prompt*/,
        const entropic::GenerationParams& /*params*/) override
    {
        return {};
    }

    int do_count_tokens(const std::string& text) const override {
        return static_cast<int>(text.size()) / 4;
    }

    entropic::LogprobResult do_evaluate_logprobs(
        const int32_t* tokens, int n_tokens) override
    {
        entropic::LogprobResult r;
        r.tokens.assign(tokens, tokens + n_tokens);
        r.n_tokens = n_tokens;
        r.n_logprobs = n_tokens - 1;
        r.logprobs.resize(
            static_cast<size_t>(n_tokens - 1), -0.5f);
        return r;
    }
};

/**
 * @brief Mock backend that declares specific capabilities.
 *
 * Supports STREAMING, RAW_COMPLETION, and TOKENIZER. Used to verify
 * that capability queries correctly reflect the override.
 *
 * @version 1.9.13
 */
class CapableMockBackend : public DefaultMockBackend {
protected:
    bool do_supports(
        entropic::BackendCapability cap) const override
    {
        switch (cap) {
        case entropic::BackendCapability::STREAMING:
        case entropic::BackendCapability::RAW_COMPLETION:
        case entropic::BackendCapability::TOKENIZER:
            return true;
        default:
            return false;
        }
    }

    entropic::BackendInfo do_info() const override {
        entropic::BackendInfo bi;
        bi.name = "capable_mock";
        bi.compute_device = "cpu";
        bi.model_format = "test";
        return bi;
    }

    bool do_clear_state(int /*seq_id*/) override {
        return true;
    }
};

/**
 * @brief Create a minimal ModelConfig for tests.
 * @return ModelConfig with test defaults.
 * @utility
 * @version 1.9.13
 */
entropic::ModelConfig make_config() {
    entropic::ModelConfig cfg;
    cfg.path = "/tmp/test.gguf";
    cfg.context_length = 4096;
    cfg.gpu_layers = -1;
    return cfg;
}

} // anonymous namespace

// ── Capability queries ────────────────────────────────────────

SCENARIO("Base class defaults to no capabilities",
         "[backend][capabilities]")
{
    GIVEN("a default mock backend") {
        DefaultMockBackend backend;

        WHEN("supports() is called for each capability") {
            THEN("all return false") {
                using C = entropic::BackendCapability;
                REQUIRE_FALSE(backend.supports(C::KV_CACHE));
                REQUIRE_FALSE(backend.supports(C::HIDDEN_STATE));
                REQUIRE_FALSE(backend.supports(C::STREAMING));
                REQUIRE_FALSE(backend.supports(C::RAW_COMPLETION));
                REQUIRE_FALSE(backend.supports(C::GRAMMAR));
                REQUIRE_FALSE(backend.supports(C::LORA_ADAPTERS));
                REQUIRE_FALSE(backend.supports(C::MULTI_SEQUENCE));
                REQUIRE_FALSE(backend.supports(C::TOKENIZER));
                REQUIRE_FALSE(backend.supports(C::LOG_PROBS));
                REQUIRE_FALSE(backend.supports(C::VISION));
                REQUIRE_FALSE(backend.supports(C::SPECULATIVE_DECODING));
                REQUIRE_FALSE(backend.supports(C::PROMPT_CACHING));
            }
        }

        WHEN("capabilities() is called") {
            auto caps = backend.capabilities();
            THEN("empty vector is returned") {
                REQUIRE(caps.empty());
            }
        }
    }
}

SCENARIO("Overridden capabilities are reported correctly",
         "[backend][capabilities]")
{
    GIVEN("a capable mock backend") {
        CapableMockBackend backend;

        WHEN("supports() is called for declared capabilities") {
            THEN("declared caps return true") {
                using C = entropic::BackendCapability;
                REQUIRE(backend.supports(C::STREAMING));
                REQUIRE(backend.supports(C::RAW_COMPLETION));
                REQUIRE(backend.supports(C::TOKENIZER));
            }
            THEN("undeclared caps return false") {
                using C = entropic::BackendCapability;
                REQUIRE_FALSE(backend.supports(C::KV_CACHE));
                REQUIRE_FALSE(backend.supports(C::GRAMMAR));
                REQUIRE_FALSE(backend.supports(C::VISION));
            }
        }

        WHEN("capabilities() is called") {
            auto caps = backend.capabilities();
            THEN("exactly 3 capabilities are returned") {
                REQUIRE(caps.size() == 3);
            }
        }
    }
}

// ── Backend info ──────────────────────────────────────────────

SCENARIO("Default backend info has name only",
         "[backend][info]")
{
    GIVEN("a default mock backend") {
        DefaultMockBackend backend;

        WHEN("info() is called") {
            auto bi = backend.info();
            THEN("name is from do_backend_name()") {
                REQUIRE(bi.name == "default_mock");
            }
            THEN("other fields are default/empty") {
                REQUIRE(bi.compute_device.empty());
                REQUIRE(bi.model_format.empty());
                REQUIRE(bi.architecture.empty());
                REQUIRE(bi.max_context_length == 0);
                REQUIRE(bi.parameter_count == 0);
                REQUIRE(bi.vram_bytes == 0);
            }
        }
    }
}

SCENARIO("Overridden backend info populates fields",
         "[backend][info]")
{
    GIVEN("a capable mock backend") {
        CapableMockBackend backend;

        WHEN("info() is called") {
            auto bi = backend.info();
            THEN("name and device are populated") {
                REQUIRE(bi.name == "capable_mock");
                REQUIRE(bi.compute_device == "cpu");
                REQUIRE(bi.model_format == "test");
            }
        }
    }
}

// ── State management defaults ─────────────────────────────────

SCENARIO("Default state operations return false",
         "[backend][state]")
{
    GIVEN("an ACTIVE default mock backend") {
        DefaultMockBackend backend;
        backend.load_and_activate(make_config());

        WHEN("save_state is called") {
            std::vector<uint8_t> buffer;
            bool ok = backend.save_state(0, buffer);
            THEN("returns false (not supported)") {
                REQUIRE_FALSE(ok);
            }
        }

        WHEN("restore_state is called") {
            std::vector<uint8_t> buffer = {1, 2, 3};
            bool ok = backend.restore_state(0, buffer);
            THEN("returns false (not supported)") {
                REQUIRE_FALSE(ok);
            }
        }

        WHEN("clear_state is called") {
            bool ok = backend.clear_state(-1);
            THEN("returns false (not supported)") {
                REQUIRE_FALSE(ok);
            }
        }
    }
}

SCENARIO("State operations require loaded model",
         "[backend][state]")
{
    GIVEN("a COLD backend") {
        DefaultMockBackend backend;

        WHEN("save_state is called") {
            std::vector<uint8_t> buffer;
            bool ok = backend.save_state(0, buffer);
            THEN("returns false") {
                REQUIRE_FALSE(ok);
            }
        }

        WHEN("clear_state is called") {
            bool ok = backend.clear_state(-1);
            THEN("returns false") {
                REQUIRE_FALSE(ok);
            }
        }
    }
}

SCENARIO("Overridden clear_state works when loaded",
         "[backend][state]")
{
    GIVEN("an ACTIVE capable mock backend") {
        CapableMockBackend backend;
        backend.load_and_activate(make_config());

        WHEN("clear_state(-1) is called") {
            bool ok = backend.clear_state(-1);
            THEN("returns true") {
                REQUIRE(ok);
            }
        }

        WHEN("clear_state(0) is called") {
            bool ok = backend.clear_state(0);
            THEN("returns true") {
                REQUIRE(ok);
            }
        }
    }
}

// ── Multi-sequence defaults ───────────────────────────────────

SCENARIO("generate_seq defaults to generate",
         "[backend][multi_sequence]")
{
    GIVEN("an ACTIVE default mock backend") {
        DefaultMockBackend backend;
        backend.load_and_activate(make_config());

        WHEN("generate_seq is called with seq_id 42") {
            auto result = backend.generate_seq(42, {}, {});
            THEN("result has content from do_generate") {
                REQUIRE(result.ok());
                REQUIRE(result.content == "default mock");
            }
            THEN("seq_id is set on result") {
                REQUIRE(result.seq_id == 42);
            }
        }
    }
}

SCENARIO("generate_seq requires ACTIVE state",
         "[backend][multi_sequence]")
{
    GIVEN("a WARM backend") {
        DefaultMockBackend backend;
        backend.load(make_config());

        WHEN("generate_seq is called") {
            auto result = backend.generate_seq(0, {}, {});
            THEN("error result is returned") {
                REQUIRE_FALSE(result.ok());
                REQUIRE(result.error_code ==
                        ENTROPIC_ERROR_INVALID_STATE);
            }
        }
    }
}

SCENARIO("generate_streaming_seq requires ACTIVE state",
         "[backend][multi_sequence]")
{
    GIVEN("a COLD backend") {
        DefaultMockBackend backend;
        std::atomic<bool> cancel{false};

        WHEN("generate_streaming_seq is called") {
            auto result = backend.generate_streaming_seq(
                0, {}, {}, nullptr, cancel);
            THEN("error result is returned") {
                REQUIRE_FALSE(result.ok());
            }
        }
    }
}

// ── GenerationResult.seq_id field ─────────────────────────────

SCENARIO("GenerationResult has seq_id field defaulting to 0",
         "[backend][generation_result]")
{
    GIVEN("a default GenerationResult") {
        entropic::GenerationResult r;
        THEN("seq_id is 0") {
            REQUIRE(r.seq_id == 0);
        }
    }
}

// ── ModelConfig.model_format field ────────────────────────────

SCENARIO("ModelConfig has model_format defaulting to gguf",
         "[backend][config]")
{
    GIVEN("a default ModelConfig") {
        entropic::ModelConfig cfg;
        THEN("model_format is gguf") {
            REQUIRE(cfg.model_format == "gguf");
        }
    }
}

// ── BackendCapability::_COUNT sentinel ────────────────────────

SCENARIO("_COUNT equals number of real capabilities",
         "[backend][capabilities]")
{
    GIVEN("the BackendCapability enum") {
        int count = static_cast<int>(
            entropic::BackendCapability::_COUNT);
        THEN("count is 12") {
            REQUIRE(count == 12);
        }
    }
}
