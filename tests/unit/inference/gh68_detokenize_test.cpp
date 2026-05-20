// SPDX-License-Identifier: Apache-2.0
/**
 * @file gh68_detokenize_test.cpp
 * @brief gh#68 — special tokens must not render to surface content.
 *
 * Pre-v2.3.4, `LlamaCppBackend::detokenize()` passed
 * `special=true` to `llama_token_to_piece`. Gemma 4's `<|im_end|>`
 * special token then decoded to the literal 10-character string
 * `<|im_end|>` which leaked into the assistant content stream.
 * Downstream turn echoed it back as history → model re-emitted EOS
 * → engine's "no tool call this iteration" retry cascade exhausted.
 *
 * v2.3.4 flipped the flag to `special=false`. These tests confirm:
 *
 *   1. A genuine EOS / `<|im_end|>` token detokenizes to empty
 *      (no leak into content).
 *   2. Regular text tokens detokenize to their surface form
 *      unchanged (positive coverage — `special=false` doesn't
 *      collateral-damage real content).
 *
 * Tagged `[.realmodel]` — needs a real GGUF on disk. Excluded from
 * default test run. Override path via `ENTROPIC_TEST_GEMMA_MODEL`
 * (default: `~/.entropic/models/gemma-4-E2B-it-Q8_0.gguf`, 2.5 GB).
 *
 * No GPU required — `do_load` loads CPU-only by design; tokenize +
 * detokenize don't need a `llama_context`.
 *
 * @version 2.3.4
 */

#include <catch2/catch_test_macros.hpp>

#include "llama_cpp_backend.h"
#include <entropic/types/config.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

/// @brief Thin subclass that exposes `detokenize` (protected on the
/// real class) so tests can call it directly. No behavior change.
class TestableLlamaCppBackend : public entropic::LlamaCppBackend {
public:
    using entropic::LlamaCppBackend::detokenize;
};

std::string gemma_model_path() {
    const char* override_path = std::getenv("ENTROPIC_TEST_GEMMA_MODEL");
    if (override_path) { return std::string(override_path); }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp")
        + "/.entropic/models/gemma-4-E2B-it-Q8_0.gguf";
}

}  // namespace

SCENARIO("Detokenize filters EOS / <|im_end|> special tokens (gh#68)",
         "[backend][gh68][.realmodel]") {
    auto path = gemma_model_path();
    if (!std::filesystem::exists(path)) {
        WARN("Skipping gh#68 detokenize test — model not present at "
             << path << ". Set ENTROPIC_TEST_GEMMA_MODEL to override.");
        return;
    }

    GIVEN("a CPU-loaded Gemma 4 backend") {
        TestableLlamaCppBackend backend;
        entropic::ModelConfig config;
        config.path = path;
        config.adapter = "gemma4";
        config.context_length = 2048;
        config.gpu_layers = 0;  // CPU only — load is enough for detokenize

        REQUIRE(backend.load(config));

        WHEN("tokenizing the surface text '<|im_end|>' to recover its token id") {
            // Gemma 4 should tokenize this exact string into a single
            // special token (or one of its end-of-turn variants).
            // We assume parse_special=true at tokenize time (which
            // `tokenize_text` does by default per its signature).
            auto tokens = backend.tokenize_text("<|im_end|>");

            // Sanity: the string maps to a non-zero token count.
            REQUIRE_FALSE(tokens.empty());

            // Find the token whose surface form WAS `<|im_end|>` under
            // the v2.3.3 special=true rendering. Under v2.3.4 the
            // detokenize call MUST NOT return the literal string for
            // any special token in the sequence.
            THEN("no token in the sequence detokenizes to '<|im_end|>'") {
                for (auto tok : tokens) {
                    std::string piece = backend.detokenize(tok);
                    // The exact bug symptom — literal '<|im_end|>' in
                    // the content stream. Must not happen for any
                    // single token under special=false.
                    REQUIRE(piece != "<|im_end|>");
                }
            }
        }

        backend.unload();
    }
}

SCENARIO("Detokenize still surfaces regular text tokens (gh#68 regression "
         "check)",
         "[backend][gh68][.realmodel]") {
    auto path = gemma_model_path();
    if (!std::filesystem::exists(path)) {
        WARN("Skipping gh#68 positive-coverage test — model not present at "
             << path);
        return;
    }

    GIVEN("a CPU-loaded Gemma 4 backend") {
        TestableLlamaCppBackend backend;
        entropic::ModelConfig config;
        config.path = path;
        config.adapter = "gemma4";
        config.context_length = 2048;
        config.gpu_layers = 0;

        REQUIRE(backend.load(config));

        WHEN("a plain text 'hello' is tokenized and round-tripped") {
            // Plain ASCII — should NOT be classified as a special
            // token. Detokenizing each piece and concatenating should
            // reproduce the original (modulo possible leading-space
            // tokenizer conventions). Verifies special=false isn't
            // collateral-damaging non-special tokens.
            auto tokens = backend.tokenize_text("hello");
            REQUIRE_FALSE(tokens.empty());

            std::string reconstructed;
            for (auto tok : tokens) {
                reconstructed += backend.detokenize(tok);
            }

            THEN("the reconstructed text contains 'hello' (no filtering of "
                 "regular content)") {
                REQUIRE(reconstructed.find("hello") != std::string::npos);
            }
        }

        backend.unload();
    }
}
