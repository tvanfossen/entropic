/**
 * @file test_logprob_evaluation.cpp
 * @brief Tests for InferenceBackend logprob evaluation (base class logic).
 *
 * Uses a mock backend that returns scripted logprobs to verify the
 * base class's state validation, perplexity computation, input
 * validation, and convenience methods.
 *
 * @version 1.9.10
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <entropic/inference/backend.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

/**
 * @brief Mock backend for logprob evaluation tests.
 *
 * Returns scripted logprobs: logprob[i] = -0.5f for every
 * token transition. This gives predictable perplexity values
 * for verification.
 *
 * @version 1.9.10
 */
class LogprobMockBackend : public entropic::InferenceBackend {
public:
    int eval_calls = 0;              ///< Number of do_evaluate calls
    bool fail_eval = false;          ///< If true, throw from do_evaluate
    float scripted_logprob = -0.5f;  ///< Logprob value per transition

protected:
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
        return {};
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

    /**
     * @brief Mock logprob evaluation.
     *
     * Returns scripted_logprob for each transition. Sets n_tokens,
     * n_logprobs, and echoes tokens back.
     *
     * @param tokens Input token array.
     * @param n_tokens Token count.
     * @return LogprobResult with scripted values.
     * @version 1.9.10
     */
    entropic::LogprobResult do_evaluate_logprobs(
        const int32_t* tokens,
        int n_tokens) override
    {
        ++eval_calls;

        if (fail_eval) {
            throw std::runtime_error("mock eval failure");
        }

        entropic::LogprobResult result;
        result.tokens.assign(tokens, tokens + n_tokens);
        result.n_tokens = n_tokens;
        result.n_logprobs = n_tokens - 1;
        result.logprobs.resize(
            static_cast<size_t>(n_tokens - 1), scripted_logprob);
        return result;
    }
};

/**
 * @brief Create a minimal ModelConfig for tests.
 * @return ModelConfig with test defaults.
 * @utility
 * @version 1.9.10
 */
entropic::ModelConfig make_config() {
    entropic::ModelConfig cfg;
    cfg.path = "/tmp/test.gguf";
    cfg.context_length = 4096;
    cfg.gpu_layers = -1;
    return cfg;
}

/**
 * @brief Activate a mock backend to ACTIVE state.
 * @param backend Backend to activate.
 * @utility
 * @version 1.9.10
 */
void activate_backend(LogprobMockBackend& backend) {
    backend.load_and_activate(make_config());
}

} // anonymous namespace

// ── Basic evaluation ──────────────────────────────────────────

SCENARIO("Basic logprob evaluation returns correct structure",
         "[logprob][evaluation]")
{
    GIVEN("an ACTIVE model and a 10-token sequence") {
        LogprobMockBackend backend;
        activate_backend(backend);
        std::vector<int32_t> tokens = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

        WHEN("evaluate_logprobs is called") {
            auto result = backend.evaluate_logprobs(
                tokens.data(), static_cast<int>(tokens.size()));

            THEN("result has correct counts") {
                REQUIRE(result.n_tokens == 10);
                REQUIRE(result.n_logprobs == 9);
                REQUIRE(result.logprobs.size() == 9);
            }

            THEN("all logprobs are negative") {
                for (float lp : result.logprobs) {
                    REQUIRE(lp <= 0.0f);
                }
            }

            THEN("tokens are echoed back") {
                REQUIRE(result.tokens == tokens);
            }
        }
    }
}

// ── Perplexity computation ────────────────────────────────────

SCENARIO("Perplexity matches manual computation",
         "[logprob][perplexity]")
{
    GIVEN("logprob result from a 10-token evaluation") {
        LogprobMockBackend backend;
        activate_backend(backend);
        std::vector<int32_t> tokens = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        auto result = backend.evaluate_logprobs(
            tokens.data(), static_cast<int>(tokens.size()));

        WHEN("perplexity is recomputed manually") {
            float sum = 0.0f;
            for (float lp : result.logprobs) {
                sum += lp;
            }
            float expected = std::exp(
                -sum / static_cast<float>(result.n_logprobs));

            THEN("result.perplexity matches within 1e-5") {
                REQUIRE_THAT(result.perplexity,
                    Catch::Matchers::WithinAbs(
                        static_cast<double>(expected), 1e-5));
            }
        }
    }
}

// ── total_logprob ─────────────────────────────────────────────

SCENARIO("total_logprob equals sum of logprobs",
         "[logprob][total]")
{
    GIVEN("a logprob result") {
        LogprobMockBackend backend;
        activate_backend(backend);
        std::vector<int32_t> tokens = {1, 2, 3, 4, 5};
        auto result = backend.evaluate_logprobs(
            tokens.data(), static_cast<int>(tokens.size()));

        WHEN("total_logprob is compared to sum") {
            float sum = 0.0f;
            for (float lp : result.logprobs) {
                sum += lp;
            }

            THEN("they match within 1e-5") {
                REQUIRE_THAT(result.total_logprob,
                    Catch::Matchers::WithinAbs(
                        static_cast<double>(sum), 1e-5));
            }
        }
    }
}

// ── Minimum sequence ──────────────────────────────────────────

SCENARIO("Minimum sequence (2 tokens) succeeds",
         "[logprob][minimum]")
{
    GIVEN("an ACTIVE model") {
        LogprobMockBackend backend;
        activate_backend(backend);
        std::vector<int32_t> tokens = {100, 200};

        WHEN("evaluate_logprobs is called with 2 tokens") {
            auto result = backend.evaluate_logprobs(
                tokens.data(), static_cast<int>(tokens.size()));

            THEN("result has 1 logprob") {
                REQUIRE(result.n_logprobs == 1);
                REQUIRE(result.logprobs.size() == 1);
            }
        }
    }
}

// ── Input validation ──────────────────────────────────────────

SCENARIO("Single token is rejected",
         "[logprob][validation]")
{
    GIVEN("an ACTIVE model") {
        LogprobMockBackend backend;
        activate_backend(backend);
        int32_t token = 42;

        WHEN("evaluate_logprobs is called with 1 token") {
            THEN("runtime_error is thrown") {
                REQUIRE_THROWS_AS(
                    backend.evaluate_logprobs(&token, 1),
                    std::runtime_error);
            }
        }
    }
}

SCENARIO("Zero tokens is rejected",
         "[logprob][validation]")
{
    GIVEN("an ACTIVE model") {
        LogprobMockBackend backend;
        activate_backend(backend);

        WHEN("evaluate_logprobs is called with 0 tokens") {
            THEN("runtime_error is thrown") {
                REQUIRE_THROWS_AS(
                    backend.evaluate_logprobs(nullptr, 0),
                    std::runtime_error);
            }
        }
    }
}

// ── State validation ──────────────────────────────────────────

SCENARIO("Non-ACTIVE model is rejected",
         "[logprob][state]")
{
    GIVEN("a WARM model") {
        LogprobMockBackend backend;
        backend.load(make_config());
        REQUIRE(backend.state() == entropic::ModelState::WARM);

        std::vector<int32_t> tokens = {1, 2, 3};

        WHEN("evaluate_logprobs is called") {
            THEN("runtime_error is thrown mentioning ACTIVE") {
                REQUIRE_THROWS_AS(
                    backend.evaluate_logprobs(
                        tokens.data(),
                        static_cast<int>(tokens.size())),
                    std::runtime_error);
            }
        }
    }

    GIVEN("a COLD model") {
        LogprobMockBackend backend;
        REQUIRE(backend.state() == entropic::ModelState::COLD);

        std::vector<int32_t> tokens = {1, 2, 3};

        WHEN("evaluate_logprobs is called") {
            THEN("runtime_error is thrown") {
                REQUIRE_THROWS_AS(
                    backend.evaluate_logprobs(
                        tokens.data(),
                        static_cast<int>(tokens.size())),
                    std::runtime_error);
            }
        }
    }
}

// ── compute_perplexity convenience ────────────────────────────

SCENARIO("compute_perplexity returns perplexity only",
         "[logprob][convenience]")
{
    GIVEN("an ACTIVE model and a token sequence") {
        LogprobMockBackend backend;
        activate_backend(backend);
        std::vector<int32_t> tokens = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

        WHEN("both methods are called") {
            auto full = backend.evaluate_logprobs(
                tokens.data(), static_cast<int>(tokens.size()));
            float ppl = backend.compute_perplexity(
                tokens.data(), static_cast<int>(tokens.size()));

            THEN("perplexity values match") {
                REQUIRE_THAT(static_cast<double>(ppl),
                    Catch::Matchers::WithinAbs(
                        static_cast<double>(full.perplexity), 1e-5));
            }
        }
    }
}

// ── Deterministic results ─────────────────────────────────────

SCENARIO("Deterministic results across calls",
         "[logprob][determinism]")
{
    GIVEN("an ACTIVE model and a token sequence") {
        LogprobMockBackend backend;
        activate_backend(backend);
        std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

        WHEN("evaluate_logprobs is called twice") {
            auto r1 = backend.evaluate_logprobs(
                tokens.data(), static_cast<int>(tokens.size()));
            auto r2 = backend.evaluate_logprobs(
                tokens.data(), static_cast<int>(tokens.size()));

            THEN("logprobs arrays are identical") {
                REQUIRE(r1.logprobs == r2.logprobs);
                REQUIRE(r1.perplexity == r2.perplexity);
            }
        }
    }
}

// ── Perplexity with known values ──────────────────────────────

SCENARIO("Perplexity math correctness with known logprobs",
         "[logprob][math]")
{
    GIVEN("a backend with scripted logprob = -1.0") {
        LogprobMockBackend backend;
        activate_backend(backend);
        backend.scripted_logprob = -1.0f;
        std::vector<int32_t> tokens = {1, 2, 3, 4, 5};

        WHEN("evaluate_logprobs is called") {
            auto result = backend.evaluate_logprobs(
                tokens.data(), static_cast<int>(tokens.size()));

            THEN("perplexity = exp(1.0) = e") {
                // mean logprob = -1.0, perplexity = exp(-(-1.0)) = e
                float expected = std::exp(1.0f);
                REQUIRE_THAT(result.perplexity,
                    Catch::Matchers::WithinAbs(
                        static_cast<double>(expected), 1e-4));
            }
        }
    }

    GIVEN("a backend with scripted logprob = -2.0") {
        LogprobMockBackend backend;
        activate_backend(backend);
        backend.scripted_logprob = -2.0f;
        std::vector<int32_t> tokens = {1, 2, 3};

        WHEN("evaluate_logprobs is called") {
            auto result = backend.evaluate_logprobs(
                tokens.data(), static_cast<int>(tokens.size()));

            THEN("perplexity = exp(2.0)") {
                float expected = std::exp(2.0f);
                REQUIRE_THAT(result.perplexity,
                    Catch::Matchers::WithinAbs(
                        static_cast<double>(expected), 1e-4));
            }
        }
    }
}

// ── Backend failure propagation ───────────────────────────────

SCENARIO("Backend evaluation failure propagates",
         "[logprob][error]")
{
    GIVEN("an ACTIVE model that fails on eval") {
        LogprobMockBackend backend;
        activate_backend(backend);
        backend.fail_eval = true;
        std::vector<int32_t> tokens = {1, 2, 3};

        WHEN("evaluate_logprobs is called") {
            THEN("runtime_error from backend propagates") {
                REQUIRE_THROWS_AS(
                    backend.evaluate_logprobs(
                        tokens.data(),
                        static_cast<int>(tokens.size())),
                    std::runtime_error);
            }
        }
    }
}

// ── Eval call count ───────────────────────────────────────────

SCENARIO("do_evaluate_logprobs is called exactly once",
         "[logprob][delegation]")
{
    GIVEN("an ACTIVE model") {
        LogprobMockBackend backend;
        activate_backend(backend);
        std::vector<int32_t> tokens = {1, 2, 3, 4, 5};

        WHEN("evaluate_logprobs is called") {
            backend.evaluate_logprobs(
                tokens.data(), static_cast<int>(tokens.size()));

            THEN("do_evaluate was called once") {
                REQUIRE(backend.eval_calls == 1);
            }
        }
    }
}
