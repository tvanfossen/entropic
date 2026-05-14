// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file speculative_compat.cpp
 * @brief Implementation of the target/draft compatibility check.
 *
 * Mirrors the file-private `common_speculative_are_compatible`
 * function from `extern/llama.cpp/common/speculative.cpp` plus an
 * entropic-side recurrent-architecture gate. See the header for the
 * full rationale.
 *
 * @version 2.1.11
 */

#include <entropic/inference/speculative_compat.h>

#include <llama.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>

namespace entropic::speculative {

namespace {

// Mirrors llama.cpp's constants. Keep in sync with
// extern/llama.cpp/common/speculative.cpp if upstream tightens.
constexpr int kSpecVocabMaxSizeDifference = 128;
constexpr int kSpecVocabCheckStartTokenId = 5;

/**
 * @brief Refuse speculative pairing for recurrent / hybrid-Mamba
 *        target architectures.
 *
 * @param target Target llama_model.
 * @return Diagnostic string if rejected, empty optional otherwise.
 * @internal
 * @version 2.1.11
 */
std::optional<std::string> check_recurrent_gate(
    const llama_model* target) {
    if (llama_model_is_recurrent(target)) {
        return std::string{
            "target model is recurrent (Mamba/RWKV/hybrid) — "
            "speculative decoding is incompatible with recurrent "
            "architectures at the v2.1.11 llama.cpp pin"};
    }
    return std::nullopt;
}

/**
 * @brief Compare llama_vocab_type between target and draft.
 *
 * @param vt Target vocab.
 * @param vd Draft vocab.
 * @return Diagnostic string on mismatch.
 * @internal
 * @version 2.1.11
 */
std::optional<std::string> check_vocab_type(
    const llama_vocab* vt, const llama_vocab* vd) {
    if (llama_vocab_type(vt) != llama_vocab_type(vd)) {
        return std::string{
            "vocab type differs between target and draft models"};
    }
    return std::nullopt;
}

/**
 * @brief Compare BOS-add behavior and BOS token id.
 *
 * @param vt Target vocab.
 * @param vd Draft vocab.
 * @return Diagnostic string on mismatch.
 * @internal
 * @version 2.1.11
 */
std::optional<std::string> check_bos(
    const llama_vocab* vt, const llama_vocab* vd) {
    const bool add_t = llama_vocab_get_add_bos(vt);
    const bool add_d = llama_vocab_get_add_bos(vd);
    if (add_t != add_d) {
        return std::string{"BOS add-behavior differs"};
    }
    if (add_t && llama_vocab_bos(vt) != llama_vocab_bos(vd)) {
        return std::string{"BOS token id differs"};
    }
    return std::nullopt;
}

/**
 * @brief Compare EOS-add behavior and EOS token id.
 *
 * @param vt Target vocab.
 * @param vd Draft vocab.
 * @return Diagnostic string on mismatch.
 * @internal
 * @version 2.1.11
 */
std::optional<std::string> check_eos(
    const llama_vocab* vt, const llama_vocab* vd) {
    const bool add_t = llama_vocab_get_add_eos(vt);
    const bool add_d = llama_vocab_get_add_eos(vd);
    if (add_t != add_d) {
        return std::string{"EOS add-behavior differs"};
    }
    if (add_t && llama_vocab_eos(vt) != llama_vocab_eos(vd)) {
        return std::string{"EOS token id differs"};
    }
    return std::nullopt;
}

/**
 * @brief Compare vocab sizes within tolerance.
 *
 * @param vt Target vocab.
 * @param vd Draft vocab.
 * @return Diagnostic string when the absolute difference exceeds
 *         `kSpecVocabMaxSizeDifference` (128).
 * @internal
 * @version 2.1.11
 */
std::optional<std::string> check_vocab_size(
    const llama_vocab* vt, const llama_vocab* vd) {
    const int nt = llama_vocab_n_tokens(vt);
    const int nd = llama_vocab_n_tokens(vd);
    const int diff = (nt > nd) ? (nt - nd) : (nd - nt);
    if (diff > kSpecVocabMaxSizeDifference) {
        return std::string{
            "vocab size difference exceeds the speculative tolerance "
            "(target=" + std::to_string(nt)
            + ", draft=" + std::to_string(nd)
            + ", max-allowed-diff="
            + std::to_string(kSpecVocabMaxSizeDifference) + ")"};
    }
    return std::nullopt;
}

/**
 * @brief Verify token-text equality for the overlap range
 *        `[5, min(n_vocab_tgt, n_vocab_dft))`.
 *
 * Skips the first 5 token ids (BOS/EOS/UNK/PAD region) — same
 * convention as upstream.
 *
 * @param vt Target vocab.
 * @param vd Draft vocab.
 * @return Diagnostic string on first mismatch, empty otherwise.
 * @internal
 * @version 2.1.11
 */
std::optional<std::string> check_token_text(
    const llama_vocab* vt, const llama_vocab* vd) {
    const int nt = llama_vocab_n_tokens(vt);
    const int nd = llama_vocab_n_tokens(vd);
    const int end = std::min(nt, nd);
    for (int i = kSpecVocabCheckStartTokenId; i < end; ++i) {
        const char* tt = llama_vocab_get_text(vt, i);
        const char* td = llama_vocab_get_text(vd, i);
        if (tt == nullptr || td == nullptr || std::strcmp(tt, td) != 0) {
            return std::string{
                "token text differs at id "
                + std::to_string(i)
                + " — draft tokenizer is not a prefix-compatible "
                  "subset of the target"};
        }
    }
    return std::nullopt;
}

} // anonymous namespace

namespace {

/**
 * @brief Walk the vocab-level checks against an already-validated
 *        target/draft pair and return the first failure diagnostic.
 *
 * Pre: target/draft non-null, recurrent gate already passed, vocabs
 * already resolved non-null. Keeps the public orchestrator under the
 * `returns ≤ 3` quality gate.
 *
 * @param vt Target vocab.
 * @param vd Draft vocab.
 * @return Empty string on success, diagnostic on first failure.
 * @internal
 * @version 2.1.11
 */
std::string run_vocab_checks(
    const llama_vocab* vt, const llama_vocab* vd) {
    using Check = std::optional<std::string> (*)(
        const llama_vocab*, const llama_vocab*);
    static constexpr Check checks[] = {
        &check_vocab_type, &check_bos, &check_eos,
        &check_vocab_size, &check_token_text,
    };
    std::string err;
    for (Check fn : checks) {
        if (err.empty()) {
            auto d = fn(vt, vd);
            if (d.has_value()) { err = std::move(*d); }
        }
    }
    return err;
}

/**
 * @brief Produce a diagnostic string for the target+draft pair, or
 *        empty on full success.
 *
 * Accumulator pattern keeps `check_compat` itself at a single return.
 *
 * @param target Target model.
 * @param draft  Draft model.
 * @return Diagnostic string ("" = compatible).
 * @internal
 * @version 2.1.11
 */
std::string build_compat_diagnostic(
    const llama_model* target, const llama_model* draft) {
    std::string err;
    if (target == nullptr || draft == nullptr) {
        err = "null model handle (target or draft)";
    } else if (auto d = check_recurrent_gate(target); d.has_value()) {
        err = std::move(*d);
    } else {
        const llama_vocab* vt = llama_model_get_vocab(target);
        const llama_vocab* vd = llama_model_get_vocab(draft);
        err = (vt == nullptr || vd == nullptr)
                  ? std::string{"model vocab unavailable (target or draft)"}
                  : run_vocab_checks(vt, vd);
    }
    return err;
}

} // anonymous namespace

/**
 * @brief Compatibility orchestrator.
 *
 * @param target Target (verifier) model.
 * @param draft  Draft (proposer) model.
 * @return CompatResult.
 * @utility
 * @version 2.1.11
 */
CompatResult check_compat(
    const llama_model* target, const llama_model* draft) {
    std::string err = build_compat_diagnostic(target, draft);
    return CompatResult{err.empty(), std::move(err)};
}

} // namespace entropic::speculative
