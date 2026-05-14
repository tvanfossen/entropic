// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file speculative_compat.h
 * @brief Tokenizer/architecture compatibility check for speculative
 *        decoding draft pairing.
 *
 * @par Why this lives in entropic, not llama.cpp
 * At the v2.1.11-pinned commit (`253ba110b`), llama.cpp's vocab
 * compatibility check moved from a public symbol
 * (`common_speculative_is_compat`, exposed at the older `7f2cbd9a4`
 * pin) to a file-private `static bool common_speculative_are_compatible`
 * inside `extern/llama.cpp/common/speculative.cpp`. The check is
 * exercised implicitly by `common_speculative_impl_draft_simple`'s
 * constructor, which **throws `std::runtime_error`** on mismatch — no
 * query-without-commit path remains in the public C++ API.
 *
 * Mirroring the logic in entropic gives us:
 *   - A metadata-only compatibility query (no llama_context allocation
 *     just to learn whether a pairing is viable).
 *   - An explicit `entropic_speculative_compat` C ABI entry point
 *     for downstream consumers (see `entropic/entropic.h`).
 *   - Unit-testability against mock vocabs on CPU pre-commit.
 *
 * In addition to the vocab-level checks llama.cpp's static helper
 * performs, entropic adds an explicit architecture gate that refuses
 * speculative pairing for both recurrent (Mamba/RWKV) AND hybrid
 * (Jamba/Granite/Nemotron-H/QWEN35/QWEN35MOE/QWEN3NEXT/KIMI_LINEAR/
 * FALCON_H1/PLAMO2/LFM2/LFM2MOE) target architectures.
 *
 * Upstream's speculative layer does NOT self-disable for either
 * recurrent or hybrid targets at this pin. The speculative-decoding
 * math assumes a non-recurrent verifier; Gate-A diagnostics in
 * Session 5 (proposal Implementation Log) showed empirically that
 * hybrid-SSM targets produce totally divergent logits at the first
 * speculative-batch boundary because the chunked SSM scan does not
 * carry the recurrent state across ubatch boundaries. The guard
 * refuses both categories so consumers fall back to plain decode
 * cleanly instead of generating garbage.
 *
 * @version 2.1.11
 */

#pragma once

#include <string>

struct llama_model;

namespace entropic::speculative {

/**
 * @brief Result of a draft/target compatibility check.
 *
 * @version 2.1.11
 */
struct CompatResult {
    bool compatible = false;       ///< true when draft can pair with target
    std::string diagnostic;        ///< Human-readable reason on failure
                                   ///< (empty when compatible).
};

/**
 * @brief Check whether a draft model can pair with a target for
 *        sequential speculative decoding.
 *
 * Mirrors the logic of llama.cpp's file-private
 * `common_speculative_are_compatible` (in `common/speculative.cpp`)
 * and additionally enforces entropic's architecture gate:
 *
 *   1. Target model must NOT be recurrent (Mamba/RWKV) AND must NOT
 *      be hybrid (Jamba, Granite-Hybrid, Nemotron-H, QWEN35/QWEN35MOE,
 *      QWEN3NEXT, KIMI_LINEAR, FALCON_H1, PLAMO2, LFM2/LFM2MOE).
 *      Speculative-decoding math assumes a pure-transformer verifier;
 *      upstream does not self-disable for these architectures at the
 *      v2.1.11 pin, and hybrid SSM targets produce divergent logits
 *      across split-prefill boundaries (Gate A, Session 5).
 *   2. Vocab type must match between target and draft.
 *   3. BOS-add behavior and BOS token id must match.
 *   4. EOS-add behavior and EOS token id must match.
 *   5. Vocab size difference must be ≤ 128 tokens
 *      (`SPEC_VOCAB_MAX_SIZE_DIFFERENCE` in llama.cpp).
 *   6. Token text must match for tokens
 *      `[SPEC_VOCAB_CHECK_START_TOKEN_ID=5, min(n_vocab_tgt, n_vocab_dft))`.
 *
 * @param target Target (verifier) llama_model handle. Must be non-null.
 * @param draft  Draft (proposer) llama_model handle. Must be non-null.
 * @return CompatResult — `compatible=true` and empty diagnostic on
 *         success; `compatible=false` with a specific diagnostic
 *         string identifying the first failed rule on failure.
 * @utility
 * @version 2.1.11
 */
CompatResult check_compat(
    const llama_model* target, const llama_model* draft);

} // namespace entropic::speculative
