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
 * @internal
 * @version 2.3.10
 */

#include "llama_cpp_tokenizer.h"
#include <entropic/types/logging.h>

#include <llama.h>

#include <vector>

static auto logger = entropic::log::get("inference.tokenizer");

namespace entropic {

/**
 * @brief Construct a tokenizer borrowing a llama_vocab pointer.
 *
 * vocab is borrowed, not owned — its lifetime must outlive this
 * object. LlamaCppBackend resets the tokenizer before freeing the
 * backing llama_model so the borrow never dangles.
 * @internal
 * @version 2.3.10
 */
LlamaCppTokenizer::LlamaCppTokenizer(const llama_vocab* vocab)
    : vocab_(vocab) {}

/**
 * @brief Encode text into token ids via the wrapped vocab.
 * @param text Input string (any UTF-8).
 * @param add_special True to prepend BOS / model-defined special tokens.
 * @return Token id vector. Empty if vocab_ is null or llama_tokenize
 *         returned a negative actual-count on the sized retry.
 * @internal
 * @version 2.3.10
 */
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

/**
 * @brief Decode a single token id to its surface string.
 *
 * special=false — special tokens (BOS, EOS, channel markers) do not
 * render to surface text. History (gh#68, gh#65): defensive flag is
 * kept regardless of whether the current model fleet emits special
 * tokens, since the seam is shared across families.
 * @param token Token id to decode.
 * @return Surface string. Empty when vocab_ is null or the retry
 *         decode returned non-positive.
 * @internal
 * @version 2.3.10
 */
std::string LlamaCppTokenizer::detokenize(int32_t token) const {
    if (vocab_ == nullptr) { return {}; }

    // special=false — special tokens don't render to surface text.
    // History (gh#68, gh#65): defensive flag, kept regardless of
    // whether the current model fleet emits special tokens.
    char buf[256];
    int n = llama_token_to_piece(
        vocab_, token, buf, sizeof(buf), 0, false);
    if (n >= 0) { return std::string(buf, static_cast<size_t>(n)); }

    // Buffer too small — retry with exact size. n holds -required_size.
    std::vector<char> large(static_cast<size_t>(-n));
    n = llama_token_to_piece(vocab_, token, large.data(),
                             static_cast<int32_t>(large.size()),
                             0, false);
    return n > 0
        ? std::string(large.data(), static_cast<size_t>(n))
        : std::string{};
}

} // namespace entropic
