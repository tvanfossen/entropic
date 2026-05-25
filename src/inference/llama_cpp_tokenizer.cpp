// SPDX-License-Identifier: Apache-2.0
/**
 * @file llama_cpp_tokenizer.cpp
 * @brief LlamaCppTokenizer implementation (v2.3.10 seam impl).
 *
 * Forwards to `llama_tokenize` / `llama_token_to_piece`. This file
 * is uncovered by the CPU-only unit-test gate by design — its only
 * job is to be a thin pass-through to llama.cpp. Coverage of the
 * LOGIC that consumes these results lives in LlamaCppBackend tests
 * (which mock the Tokenizer interface).
 *
 * @internal
 * @version 2.3.10
 */

#include "llama_cpp_tokenizer.h"
#include <entropic/types/logging.h>

#include <llama.h>

#include <vector>

static auto logger = entropic::log::get("inference.tokenizer");

namespace entropic {

LlamaCppTokenizer::LlamaCppTokenizer(const llama_vocab* vocab)
    : vocab_(vocab) {}

std::vector<int32_t> LlamaCppTokenizer::tokenize(
    const std::string& text, bool add_special) const
{
    if (vocab_ == nullptr) { return {}; }

    // First call: get required size (negative return = required size).
    int n = llama_tokenize(vocab_, text.c_str(),
                           static_cast<int32_t>(text.size()),
                           nullptr, 0, add_special, true);
    if (n < 0) { n = -n; }

    std::vector<int32_t> tokens(static_cast<size_t>(n));
    int actual = llama_tokenize(vocab_, text.c_str(),
                                static_cast<int32_t>(text.size()),
                                tokens.data(), n, add_special, true);
    if (actual < 0) {
        logger->error("Tokenization failed for text of length {}",
                      text.size());
        return {};
    }
    tokens.resize(static_cast<size_t>(actual));
    return tokens;
}

std::string LlamaCppTokenizer::detokenize(int32_t token) const {
    if (vocab_ == nullptr) { return {}; }

    // special=false — special tokens don't render to surface text.
    // History (gh#68, gh#65): defensive flag, kept regardless of
    // whether the current model fleet emits special tokens.
    char buf[256];
    int n = llama_token_to_piece(
        vocab_, token, buf, sizeof(buf), 0, false);
    if (n < 0) {
        // Buffer too small — retry with exact size.
        std::vector<char> large(static_cast<size_t>(-n));
        n = llama_token_to_piece(vocab_, token, large.data(),
                                 static_cast<int32_t>(large.size()),
                                 0, false);
        if (n > 0) {
            return std::string(large.data(), static_cast<size_t>(n));
        }
        return "";
    }
    return std::string(buf, static_cast<size_t>(n));
}

} // namespace entropic
