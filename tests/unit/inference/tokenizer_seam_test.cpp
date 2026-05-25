// SPDX-License-Identifier: Apache-2.0
/**
 * @file tokenizer_seam_test.cpp
 * @brief Exercises the v2.3.10 LlamaCppBackend → Tokenizer seam.
 *
 * Pre-v2.3.10 every tokenize/detokenize/count_tokens path on
 * LlamaCppBackend made inline llama.cpp calls and was unreachable
 * under the CPU-only unit-test coverage gate. v2.3.10 introduces
 * an abstract Tokenizer (include/entropic/inference/tokenizer.h)
 * with a production impl (LlamaCppTokenizer) and a test-only
 * injection point (LlamaCppBackend::inject_tokenizer_for_test).
 *
 * This test wires a MockTokenizer and asserts the backend routes
 * count_tokens / tokenize_text / detokenize through the mock.
 *
 * @version 2.3.10
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/inference/tokenizer.h>
#include "../../../src/inference/llama_cpp_backend.h"

#include <string>
#include <vector>

namespace {

/**
 * @brief Capturing mock tokenizer for unit tests.
 *
 * Records every call so scenarios can assert on the inputs the
 * backend forwards. Returns programmable canned results so each
 * test pins the contract end-to-end.
 *
 * @internal
 * @version 2.3.10
 */
class MockTokenizer : public entropic::Tokenizer {
public:
    // Programmable results.
    std::vector<int32_t> next_tokenize_result;
    std::string next_detokenize_result;

    // Captured calls.
    mutable int tokenize_calls = 0;
    mutable int detokenize_calls = 0;
    mutable int count_tokens_calls = 0;
    mutable std::string last_text;
    mutable bool last_add_special = false;
    mutable int32_t last_token = -1;

    std::vector<int32_t> tokenize(
        const std::string& text, bool add_special) const override {
        ++tokenize_calls;
        last_text = text;
        last_add_special = add_special;
        return next_tokenize_result;
    }

    std::string detokenize(int32_t token) const override {
        ++detokenize_calls;
        last_token = token;
        return next_detokenize_result;
    }

    int count_tokens(const std::string& text) const override {
        ++count_tokens_calls;
        last_text = text;
        // Defer to default behavior so we cover the default impl too.
        return entropic::Tokenizer::count_tokens(text);
    }
};

} // anonymous namespace

// ── Tokenizer abstract default impl ─────────────────────────

SCENARIO("Tokenizer::count_tokens default impl returns tokenize().size()",
         "[tokenizer][v2.3.10][seam]")
{
    MockTokenizer mock;

    GIVEN("a tokenizer programmed to return 4 ids") {
        mock.next_tokenize_result = {1, 2, 3, 4};

        WHEN("count_tokens is invoked") {
            int n = mock.count_tokens("any");
            THEN("the count matches tokenize().size()") {
                REQUIRE(n == 4);
                REQUIRE(mock.count_tokens_calls == 1);
                REQUIRE(mock.tokenize_calls == 1);
                REQUIRE_FALSE(mock.last_add_special);
            }
        }
    }

    GIVEN("a tokenizer that returns an empty vector") {
        mock.next_tokenize_result = {};
        WHEN("count_tokens is invoked") {
            int n = mock.count_tokens("");
            THEN("count is 0") {
                REQUIRE(n == 0);
            }
        }
    }
}

// ── LlamaCppBackend seam ────────────────────────────────────

SCENARIO("inject_tokenizer_for_test flips state and routes count_tokens",
         "[tokenizer][v2.3.10][seam][backend]")
{
    entropic::LlamaCppBackend backend;
    auto mock = std::make_unique<MockTokenizer>();
    auto* mock_raw = mock.get();
    mock_raw->next_tokenize_result = {10, 20, 30, 40, 50};

    GIVEN("a backend with an injected mock tokenizer") {
        backend.inject_tokenizer_for_test(std::move(mock));

        THEN("state is WARM (so is_loaded() returns true)") {
            REQUIRE(backend.state() == entropic::ModelState::WARM);
            REQUIRE(backend.is_loaded());
        }

        WHEN("count_tokens is called on the backend") {
            int n = backend.count_tokens("five-token text");
            THEN("the call routes through the mock") {
                REQUIRE(n == 5);
                // count_tokens on the backend goes through
                // InferenceBackend::count_tokens → do_count_tokens →
                // LlamaCppBackend::do_count_tokens → tokenize() →
                // tokenizer_->tokenize(). The mock's tokenize was hit.
                REQUIRE(mock_raw->tokenize_calls >= 1);
                REQUIRE(mock_raw->last_text == "five-token text");
                REQUIRE_FALSE(mock_raw->last_add_special);
            }
        }
    }
}

SCENARIO("Backend tokenize_text routes through the Tokenizer mock",
         "[tokenizer][v2.3.10][seam][backend]")
{
    entropic::LlamaCppBackend backend;
    auto mock = std::make_unique<MockTokenizer>();
    auto* mock_raw = mock.get();
    mock_raw->next_tokenize_result = {7, 11, 13};

    backend.inject_tokenizer_for_test(std::move(mock));

    GIVEN("a configured backend with a mock tokenizer") {
        WHEN("tokenize_text is called with arbitrary text") {
            auto ids = backend.tokenize_text("primes");
            THEN("the mock returns the programmed token ids") {
                REQUIRE(ids.size() == 3);
                REQUIRE(ids[0] == 7);
                REQUIRE(ids[1] == 11);
                REQUIRE(ids[2] == 13);
                // tokenize_text sets add_special=true (BOS).
                REQUIRE(mock_raw->last_add_special);
                REQUIRE(mock_raw->last_text == "primes");
            }
        }
    }
}

SCENARIO("Backend count_tokens fallback when no tokenizer injected",
         "[tokenizer][v2.3.10][seam][backend][failure-mode]")
{
    entropic::LlamaCppBackend backend;
    // No injection — backend stays COLD, tokenizer_ is nullptr.

    GIVEN("a COLD backend without a tokenizer") {
        THEN("state is COLD") {
            REQUIRE(backend.state() == entropic::ModelState::COLD);
            REQUIRE_FALSE(backend.is_loaded());
        }

        WHEN("count_tokens is called on a COLD backend") {
            int n = backend.count_tokens("twelve chars");
            THEN("the base-class fallback returns size()/4") {
                // base class fallback: not is_loaded() ⇒ text.size()/4
                REQUIRE(n == static_cast<int>(std::strlen("twelve chars")) / 4);
            }
        }
    }
}

SCENARIO("Backend tokenize_text returns empty when no tokenizer wired",
         "[tokenizer][v2.3.10][seam][backend][failure-mode]")
{
    entropic::LlamaCppBackend backend;

    WHEN("tokenize_text is called without injection") {
        auto ids = backend.tokenize_text("anything");
        THEN("an empty vector is returned (private tokenize() short-circuits)") {
            // Private LlamaCppBackend::tokenize returns {} when
            // tokenizer_ is null. tokenize_text passes that through.
            REQUIRE(ids.empty());
        }
    }
}

SCENARIO("Multiple count_tokens calls accumulate mock invocations",
         "[tokenizer][v2.3.10][seam][backend]")
{
    entropic::LlamaCppBackend backend;
    auto mock = std::make_unique<MockTokenizer>();
    auto* mock_raw = mock.get();
    mock_raw->next_tokenize_result = {1, 2};

    backend.inject_tokenizer_for_test(std::move(mock));

    WHEN("count_tokens is called three times in a row") {
        (void)backend.count_tokens("a");
        (void)backend.count_tokens("b");
        (void)backend.count_tokens("c");

        THEN("the mock's tokenize was invoked at least 3 times") {
            REQUIRE(mock_raw->tokenize_calls >= 3);
        }
    }
}

SCENARIO("Backend re-injection swaps the tokenizer in place",
         "[tokenizer][v2.3.10][seam][backend]")
{
    entropic::LlamaCppBackend backend;

    // First mock: returns 4 ids.
    auto first = std::make_unique<MockTokenizer>();
    first->next_tokenize_result = {1, 2, 3, 4};
    backend.inject_tokenizer_for_test(std::move(first));
    REQUIRE(backend.count_tokens("first") == 4);

    // Second mock: returns 2 ids — replaces the first cleanly.
    auto second = std::make_unique<MockTokenizer>();
    second->next_tokenize_result = {99, 100};
    backend.inject_tokenizer_for_test(std::move(second));

    WHEN("count_tokens is called after the swap") {
        int n = backend.count_tokens("second");
        THEN("the new mock's programmed result is observed") {
            REQUIRE(n == 2);
        }
    }
}
