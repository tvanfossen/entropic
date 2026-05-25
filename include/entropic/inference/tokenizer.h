// SPDX-License-Identifier: Apache-2.0
/**
 * @file tokenizer.h
 * @brief Abstract Tokenizer seam for backend testability (v2.3.10).
 *
 * Pre-v2.3.10 `LlamaCppBackend` made llama.cpp tokenizer calls inline
 * — meaning the tokenization paths were only exercisable with a
 * loaded model. The unit-test coverage gate (CPU-only, no GGUFs)
 * could never reach those lines. This abstract seam lets the
 * backend's tokenize/detokenize/count callers route through a
 * mockable interface; the production backend wires a
 * `LlamaCppTokenizer` that forwards to llama.cpp; unit tests
 * inject `MockTokenizer` and assert behavior without a model.
 *
 * @version 2.3.10
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Pure-virtual tokenizer surface used by the inference backend.
 *
 * Three responsibilities:
 *   1. Encode text → token ids (with optional BOS/EOS specials).
 *   2. Decode a single token → string piece.
 *   3. Count tokens for capacity-planning (e.g., context budget).
 *
 * Implementations may be lossy (count is exact when model is loaded,
 * estimate otherwise) but must never throw and must never crash on
 * empty inputs. Callers treat returned vectors as authoritative.
 *
 * @version 2.3.10
 */
class Tokenizer {
public:
    virtual ~Tokenizer() = default;

    /**
     * @brief Encode text to token IDs.
     * @param text UTF-8 input.
     * @param add_special Insert model-specific BOS/EOS specials when true.
     * @return Token IDs (empty on failure or empty input).
     * @version 2.3.10
     */
    virtual std::vector<int32_t> tokenize(
        const std::string& text, bool add_special) const = 0;

    /**
     * @brief Decode a single token to its surface piece.
     * @param token Token ID.
     * @return Surface string ("" for special tokens that don't render).
     * @version 2.3.10
     */
    virtual std::string detokenize(int32_t token) const = 0;

    /**
     * @brief Count tokens in text without retaining the IDs.
     *
     * Default implementation invokes tokenize() and returns size().
     * Overrides may use a faster path (e.g., model size-prediction
     * without actual allocation).
     *
     * @param text UTF-8 input.
     * @return Token count (>= 0). Returns 0 on empty input.
     * @internal
     * @version 2.3.10
     */
    virtual int count_tokens(const std::string& text) const {
        return static_cast<int>(tokenize(text, false).size());
    }
};

} // namespace entropic
