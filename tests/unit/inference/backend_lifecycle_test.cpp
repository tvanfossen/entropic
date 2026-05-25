// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_backend_lifecycle.cpp
 * @brief Tests for InferenceBackend state machine.
 *
 * Uses a mock subclass that tracks do_* calls without loading a real model.
 *
 * @version 1.8.2
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/backend.h>

#include <stdexcept>

namespace {

/**
 * @brief Mock backend that tracks lifecycle calls.
 * @version 1.9.13
 */
class MockBackend : public entropic::InferenceBackend {
public:
    int load_calls = 0;
    int activate_calls = 0;
    int deactivate_calls = 0;
    int unload_calls = 0;
    int generate_calls = 0;
    int complete_calls = 0;
    bool fail_load = false;
    bool fail_activate = false;

protected:
    std::string do_backend_name() const override { return "mock"; }

    bool do_load(const entropic::ModelConfig& /*config*/) override {
        ++load_calls;
        if (fail_load) {
            last_error_ = "mock load failure";
            return false;
        }
        return true;
    }

    bool do_activate() override {
        ++activate_calls;
        if (fail_activate) {
            last_error_ = "mock activate failure";
            return false;
        }
        return true;
    }

    void do_deactivate() override { ++deactivate_calls; }
    void do_unload() override { ++unload_calls; }

    entropic::GenerationResult do_generate(
        const std::vector<entropic::Message>& /*messages*/,
        const entropic::GenerationParams& /*params*/) override
    {
        ++generate_calls;
        entropic::GenerationResult r;
        r.content = "mock response";
        r.token_count = 2;
        return r;
    }

    entropic::GenerationResult do_generate_streaming(
        const std::vector<entropic::Message>& /*messages*/,
        const entropic::GenerationParams& /*params*/,
        std::function<void(std::string_view)> on_token,
        std::atomic<bool>& /*cancel*/) override
    {
        entropic::GenerationResult r;
        r.content = "streamed";
        if (on_token) on_token("streamed");
        r.token_count = 1;
        return r;
    }

    entropic::GenerationResult do_complete(
        const std::string& /*prompt*/,
        const entropic::GenerationParams& /*params*/) override
    {
        ++complete_calls;
        entropic::GenerationResult r;
        r.content = "2";
        r.token_count = 1;
        return r;
    }

    int do_count_tokens(const std::string& text) const override {
        return static_cast<int>(text.size()) / 4;
    }

    entropic::LogprobResult do_evaluate_logprobs(
        const int32_t* tokens,
        int n_tokens) override
    {
        entropic::LogprobResult r;
        r.tokens.assign(tokens, tokens + n_tokens);
        r.n_tokens = n_tokens;
        r.n_logprobs = n_tokens - 1;
        r.logprobs.resize(static_cast<size_t>(n_tokens - 1), -0.5f);
        return r;
    }
};

/**
 * @brief Create a minimal ModelConfig for backend lifecycle tests.
 * @return Pre-filled ModelConfig with test defaults.
 * @version 1.8.2
 * @internal
 */
entropic::ModelConfig make_config() {
    entropic::ModelConfig cfg;
    cfg.path = "/tmp/test.gguf";
    cfg.context_length = 4096;
    cfg.gpu_layers = -1;
    return cfg;
}

} // anonymous namespace

// ── Lifecycle transitions ──────────────────────────────────

SCENARIO("Backend lifecycle transitions", "[backend][lifecycle]") {
    GIVEN("a COLD backend") {
        MockBackend backend;
        REQUIRE(backend.state() == entropic::ModelState::COLD);

        WHEN("load() is called") {
            bool ok = backend.load(make_config());
            THEN("state is WARM and do_load was called") {
                REQUIRE(ok);
                REQUIRE(backend.state() == entropic::ModelState::WARM);
                REQUIRE(backend.load_calls == 1);
            }
        }
    }

    GIVEN("a WARM backend") {
        MockBackend backend;
        backend.load(make_config());

        WHEN("activate() is called") {
            bool ok = backend.activate();
            THEN("state is ACTIVE") {
                REQUIRE(ok);
                REQUIRE(backend.state() == entropic::ModelState::ACTIVE);
                REQUIRE(backend.activate_calls == 1);
            }
        }
    }

    GIVEN("an ACTIVE backend") {
        MockBackend backend;
        backend.load_and_activate(make_config());

        WHEN("deactivate() is called") {
            backend.deactivate();
            THEN("state is WARM") {
                REQUIRE(backend.state() == entropic::ModelState::WARM);
                REQUIRE(backend.deactivate_calls == 1);
            }
        }

        WHEN("unload() is called") {
            backend.unload();
            THEN("state is COLD") {
                REQUIRE(backend.state() == entropic::ModelState::COLD);
                REQUIRE(backend.unload_calls == 1);
            }
        }
    }
}

// ── Idempotent transitions ─────────────────────────────────

SCENARIO("Idempotent transitions", "[backend][lifecycle]") {
    GIVEN("a WARM backend") {
        MockBackend backend;
        backend.load(make_config());

        WHEN("load() is called again") {
            backend.load(make_config());
            THEN("do_load is NOT called again") {
                REQUIRE(backend.load_calls == 1);
                REQUIRE(backend.state() == entropic::ModelState::WARM);
            }
        }
    }

    GIVEN("an ACTIVE backend") {
        MockBackend backend;
        backend.load_and_activate(make_config());

        WHEN("activate() is called again") {
            backend.activate();
            THEN("do_activate is NOT called again") {
                REQUIRE(backend.activate_calls == 1);
            }
        }
    }

    GIVEN("a COLD backend") {
        MockBackend backend;

        WHEN("deactivate() is called") {
            backend.deactivate();
            THEN("do_deactivate is NOT called") {
                REQUIRE(backend.deactivate_calls == 0);
                REQUIRE(backend.state() == entropic::ModelState::COLD);
            }
        }
    }
}

// ── Generation requires ACTIVE ─────────────────────────────

SCENARIO("Generation requires ACTIVE state", "[backend][generate]") {
    GIVEN("a WARM backend") {
        MockBackend backend;
        backend.load(make_config());

        WHEN("generate() is called") {
            auto result = backend.generate({}, {});
            THEN("error result is returned") {
                REQUIRE_FALSE(result.ok());
                REQUIRE(result.error_code == ENTROPIC_ERROR_INVALID_STATE);
                REQUIRE(backend.generate_calls == 0);
            }
        }
    }

    GIVEN("an ACTIVE backend") {
        MockBackend backend;
        backend.load_and_activate(make_config());

        WHEN("generate() is called") {
            auto result = backend.generate({}, {});
            THEN("do_generate is called") {
                REQUIRE(result.ok());
                REQUIRE(result.content == "mock response");
                REQUIRE(backend.generate_calls == 1);
            }
        }

        WHEN("complete() is called") {
            auto result = backend.complete("test prompt", {});
            THEN("do_complete is called") {
                REQUIRE(result.ok());
                REQUIRE(result.content == "2");
                REQUIRE(backend.complete_calls == 1);
            }
        }
    }
}

// ── Convenience: load_and_activate from COLD ───────────────

SCENARIO("load_and_activate from COLD", "[backend][lifecycle]") {
    GIVEN("a COLD backend") {
        MockBackend backend;

        WHEN("load_and_activate() is called") {
            bool ok = backend.load_and_activate(make_config());
            THEN("both load and activate are called") {
                REQUIRE(ok);
                REQUIRE(backend.state() == entropic::ModelState::ACTIVE);
                REQUIRE(backend.load_calls == 1);
                REQUIRE(backend.activate_calls == 1);
            }
        }
    }
}

// ── Load failure ───────────────────────────────────────────

SCENARIO("Load failure keeps COLD state", "[backend][lifecycle]") {
    GIVEN("a backend that fails to load") {
        MockBackend backend;
        backend.fail_load = true;

        WHEN("load() is called") {
            bool ok = backend.load(make_config());
            THEN("returns false and stays COLD") {
                REQUIRE_FALSE(ok);
                REQUIRE(backend.state() == entropic::ModelState::COLD);
            }
        }
    }
}

// ── v2.3.10 [backend_topup] ────────────────────────────────
// Drives the remaining uncovered ranges in backend.cpp via a single
// mock that uses v2.3.10's protected state_ to inject ACTIVE without
// a real-model load. Targets: activate not-WARM error (96-97),
// activate failure path, generate_streaming/complete/speculative/seq
// non-ACTIVE error paths (218-262, 308-319, 571-617), default
// do_generate_speculative -> NOT_SUPPORTED (286-298), capabilities()
// iteration + info() (468-488, 647-651), save/restore/clear_state
// branches + default-false (500-558, 661-690), evaluate_logprobs
// validation + perplexity (343-406), count_tokens estimate/loaded
// branches (442-447), default do_generate_seq/streaming_seq delegates
// (701-727), load_and_activate failure short-circuit (165-168).

namespace {

class StateInjectMock : public entropic::InferenceBackend {
public:
    int generate_calls = 0;
    int streaming_calls = 0;
    int eval_calls = 0;
    bool fail_activate = false;
    bool fail_load = false;
    bool override_supports = false;
    bool override_speculative = false;
    bool override_save = false;
    bool override_restore = false;
    bool override_clear = false;

    void inject_state(entropic::ModelState s) {
        state_.store(s, std::memory_order_release);
    }

protected:
    std::string do_backend_name() const override { return "inject-mock"; }
    bool do_load(const entropic::ModelConfig&) override {
        if (fail_load) { last_error_ = "fail-load"; return false; }
        return true;
    }
    bool do_activate() override {
        if (fail_activate) { last_error_ = "fail-act"; return false; }
        return true;
    }
    void do_deactivate() override {}
    void do_unload() override {}

    entropic::GenerationResult do_generate(
        const std::vector<entropic::Message>&,
        const entropic::GenerationParams&) override {
        ++generate_calls;
        entropic::GenerationResult r; r.content = "g"; r.token_count = 1;
        return r;
    }
    entropic::GenerationResult do_generate_streaming(
        const std::vector<entropic::Message>&,
        const entropic::GenerationParams&,
        std::function<void(std::string_view)> on_token,
        std::atomic<bool>&) override {
        ++streaming_calls;
        if (on_token) on_token("s");
        entropic::GenerationResult r; r.content = "stream"; return r;
    }
    entropic::GenerationResult do_generate_speculative(
        const std::vector<entropic::Message>& m,
        const entropic::GenerationParams& p,
        std::function<void(std::string_view)> on_token,
        std::atomic<bool>& cancel) override {
        if (override_speculative) {
            entropic::GenerationResult r; r.content = "spec"; return r;
        }
        // Fall through to default impl → NOT_SUPPORTED.
        return entropic::InferenceBackend::do_generate_speculative(
            m, p, on_token, cancel);
    }
    entropic::GenerationResult do_complete(
        const std::string&,
        const entropic::GenerationParams&) override {
        entropic::GenerationResult r; r.content = "c"; return r;
    }
    int do_count_tokens(const std::string& text) const override {
        return static_cast<int>(text.size());
    }
    entropic::LogprobResult do_evaluate_logprobs(
        const int32_t* tokens, int n) override {
        ++eval_calls;
        entropic::LogprobResult r;
        r.tokens.assign(tokens, tokens + n);
        r.n_tokens = n; r.n_logprobs = n - 1;
        r.logprobs.resize(static_cast<size_t>(n - 1), -1.0f);
        return r;
    }
    bool do_supports(entropic::BackendCapability cap) const override {
        if (!override_supports) {
            return entropic::InferenceBackend::do_supports(cap);
        }
        return cap == entropic::BackendCapability::GRAMMAR;
    }
    bool do_save_state(int, std::vector<uint8_t>& buf) const override {
        if (override_save) { buf = {0x1, 0x2, 0x3}; return true; }
        return entropic::InferenceBackend::do_save_state(0, buf);
    }
    bool do_restore_state(int, const std::vector<uint8_t>&) override {
        if (override_restore) { return true; }
        std::vector<uint8_t> empty;
        return entropic::InferenceBackend::do_restore_state(0, empty);
    }
    bool do_clear_state(int) override {
        if (override_clear) { return true; }
        return entropic::InferenceBackend::do_clear_state(0);
    }
};

}  // namespace

TEST_CASE("v2.3.10 backend topup — lifecycle error paths",
          "[v2.3.10][inference][backend_topup]")
{
    // activate() from COLD without load → not-WARM error path (96-97).
    StateInjectMock be;
    REQUIRE_FALSE(be.activate());
    REQUIRE(be.state() == entropic::ModelState::COLD);

    // activate() fails when do_activate returns false (state stays WARM).
    REQUIRE(be.load(make_config()));
    be.fail_activate = true;
    REQUIRE_FALSE(be.activate());
    REQUIRE(be.state() == entropic::ModelState::WARM);

    // load_and_activate stops at load failure (state stays COLD).
    StateInjectMock m2;
    m2.fail_load = true;
    REQUIRE_FALSE(m2.load_and_activate(make_config()));
    REQUIRE(m2.state() == entropic::ModelState::COLD);

    // deactivate while WARM is a no-op (lines 120-123).
    StateInjectMock m3;
    REQUIRE(m3.load(make_config()));
    m3.deactivate();
    REQUIRE(m3.state() == entropic::ModelState::WARM);

    // unload while COLD is idempotent.
    StateInjectMock m4;
    m4.unload(); m4.unload();
    REQUIRE(m4.state() == entropic::ModelState::COLD);
}

TEST_CASE("v2.3.10 backend topup — generation rejects non-ACTIVE",
          "[v2.3.10][inference][backend_topup]")
{
    StateInjectMock be;
    REQUIRE(be.load(make_config()));  // WARM
    std::atomic<bool> cancel{false};

    auto r1 = be.generate_streaming({}, {}, [](std::string_view){}, cancel);
    REQUIRE(r1.error_code == ENTROPIC_ERROR_INVALID_STATE);
    auto r2 = be.generate_speculative({}, {}, [](std::string_view){}, cancel);
    REQUIRE(r2.error_code == ENTROPIC_ERROR_INVALID_STATE);
    auto r3 = be.complete("p", {});
    REQUIRE(r3.error_code == ENTROPIC_ERROR_INVALID_STATE);
    auto r4 = be.generate_seq(7, {}, {});
    REQUIRE(r4.error_code == ENTROPIC_ERROR_INVALID_STATE);
    auto r5 = be.generate_streaming_seq(
        7, {}, {}, [](std::string_view){}, cancel);
    REQUIRE(r5.error_code == ENTROPIC_ERROR_INVALID_STATE);
}

TEST_CASE("v2.3.10 backend topup — generation succeeds when ACTIVE",
          "[v2.3.10][inference][backend_topup]")
{
    StateInjectMock be;
    be.inject_state(entropic::ModelState::ACTIVE);
    std::atomic<bool> cancel{false};

    REQUIRE(be.generate({}, {}).ok());
    REQUIRE(be.generate_streaming(
        {}, {}, [](std::string_view){}, cancel).ok());
    REQUIRE(be.complete("p", {}).ok());

    auto r4 = be.generate_seq(42, {}, {});
    REQUIRE(r4.ok());
    REQUIRE(r4.seq_id == 42);

    auto r5 = be.generate_streaming_seq(
        43, {}, {}, [](std::string_view){}, cancel);
    REQUIRE(r5.ok());
    REQUIRE(r5.seq_id == 43);

    // Default do_generate_seq delegates to do_generate.
    REQUIRE(be.generate_calls >= 1);
    REQUIRE(be.streaming_calls >= 1);

    // Default do_generate_speculative → NOT_SUPPORTED (lines 286-298).
    auto rs = be.generate_speculative(
        {}, {}, [](std::string_view){}, cancel);
    REQUIRE(rs.error_code == ENTROPIC_ERROR_NOT_SUPPORTED);

    // Override path executes the subclass speculative impl.
    be.override_speculative = true;
    auto rs2 = be.generate_speculative(
        {}, {}, [](std::string_view){}, cancel);
    REQUIRE(rs2.ok());
    REQUIRE(rs2.content == "spec");
}

TEST_CASE("v2.3.10 backend topup — evaluate_logprobs validation + success",
          "[v2.3.10][inference][backend_topup]")
{
    StateInjectMock be;
    int32_t tokens[] = {1, 2, 3, 4};

    // COLD → throws (not-ACTIVE).
    REQUIRE_THROWS_AS(be.evaluate_logprobs(tokens, 3), std::runtime_error);

    be.inject_state(entropic::ModelState::ACTIVE);
    // n<2 → throws.
    REQUIRE_THROWS_AS(be.evaluate_logprobs(tokens, 1), std::runtime_error);

    auto r = be.evaluate_logprobs(tokens, 3);
    REQUIRE(r.n_logprobs == 2);
    REQUIRE(r.logprobs.size() == 2);
    REQUIRE(r.perplexity > 0.0f);
    REQUIRE(be.eval_calls == 1);

    // compute_perplexity wrapper.
    float pp = be.compute_perplexity(tokens, 4);
    REQUIRE(pp > 0.0f);
}

TEST_CASE("v2.3.10 backend topup — state management default + override",
          "[v2.3.10][inference][backend_topup]")
{
    StateInjectMock be;
    std::vector<uint8_t> buf;

    // COLD path for save / restore / clear.
    REQUIRE_FALSE(be.save_state(0, buf));
    REQUIRE_FALSE(be.restore_state(0, buf));
    REQUIRE_FALSE(be.clear_state(-1));

    // WARM + default-false do_clear_state.
    be.inject_state(entropic::ModelState::WARM);
    REQUIRE_FALSE(be.clear_state(0));
    be.override_clear = true;
    REQUIRE(be.clear_state(0));

    // ACTIVE: default-false do_save_state / do_restore_state.
    be.inject_state(entropic::ModelState::ACTIVE);
    REQUIRE_FALSE(be.save_state(0, buf));
    REQUIRE_FALSE(be.restore_state(0, buf));

    // ACTIVE + overrides.
    be.override_save = true;
    REQUIRE(be.save_state(0, buf));
    REQUIRE(buf.size() == 3);
    be.override_restore = true;
    REQUIRE(be.restore_state(0, buf));
}

TEST_CASE("v2.3.10 backend topup — queries, count_tokens, capabilities, info",
          "[v2.3.10][inference][backend_topup]")
{
    StateInjectMock be;

    // count_tokens COLD path (text.size()/4 estimate).
    REQUIRE(be.count_tokens("abcdefgh") == 2);

    // count_tokens loaded path (do_count_tokens).
    be.inject_state(entropic::ModelState::WARM);
    REQUIRE(be.count_tokens("hi!") == 3);

    // capabilities() with default do_supports=false.
    StateInjectMock be2;
    REQUIRE(be2.capabilities().empty());
    REQUIRE_FALSE(be2.supports(entropic::BackendCapability::GRAMMAR));

    // capabilities() with overridden do_supports.
    be2.override_supports = true;
    auto caps = be2.capabilities();
    REQUIRE(caps.size() == 1);
    REQUIRE(caps[0] == entropic::BackendCapability::GRAMMAR);

    // info() default impl populates name from do_backend_name (647-651).
    auto bi = be.info();
    REQUIRE(bi.name == "inject-mock");

    // tokenize_text default returns empty.
    REQUIRE(be.tokenize_text("hello").empty());

    // clear_prompt_cache default impl is a no-op.
    be.clear_prompt_cache();

    // config() / context_length() expose stored config.
    StateInjectMock be3;
    auto cfg = make_config();
    cfg.context_length = 8192;
    REQUIRE(be3.load(cfg));
    REQUIRE(be3.config().context_length == 8192);
    REQUIRE(be3.context_length() == 8192);
}
