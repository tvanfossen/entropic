// SPDX-License-Identifier: Apache-2.0
/**
 * @file llama_cpp_tokenizer.h
 * @brief Concrete llama.cpp tokenizer (v2.3.10 seam impl).
 *
 * Implements the abstract Tokenizer interface against a borrowed
 * `llama_vocab*` (owned by the model). Lifetime: must be released
 * before the owning model is freed; LlamaCppBackend handles that
 * ordering. Tokenizer methods never throw; on llama.cpp error they
 * log and return empty/0.
 *
 * @internal
 * @version 2.3.10
 */

#pragma once

#include <entropic/inference/tokenizer.h>

// Forward-declare the opaque llama vocab — keeps llama.h out of the
// public header surface. The .cpp pulls in llama.h for real calls.
struct llama_vocab;

namespace entropic {

/**
 * @brief Tokenizer adapter that forwards to llama.cpp's vocab API.
 *
 * Borrowing semantics: vocab pointer is owned by the model. Caller
 * (LlamaCppBackend) must ensure the model outlives the tokenizer.
 *
 * @internal
 * @version 2.3.10
 */
class LlamaCppTokenizer : public Tokenizer {
public:
    /**
     * @brief Construct with a borrowed vocab pointer.
     * @param vocab Non-owning pointer; must outlive this tokenizer.
     * @version 2.3.10
     */
    explicit LlamaCppTokenizer(const llama_vocab* vocab);

    std::vector<int32_t> tokenize(
        const std::string& text, bool add_special) const override;

    std::string detokenize(int32_t token) const override;

private:
    const llama_vocab* vocab_;
};

} // namespace entropic
