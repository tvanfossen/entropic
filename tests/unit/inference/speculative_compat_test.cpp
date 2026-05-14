// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file speculative_compat_test.cpp
 * @brief Unit tests for entropic::speculative::check_compat.
 *
 * Mocks the llama.cpp vocab/model accessors at link time so the
 * compatibility-check logic can be exercised on CPU without loading
 * any GGUFs. Each test case constructs a (target, draft) pair of
 * sentinel `llama_model*` pointers, configures the mock to return
 * specific vocab attributes for each, and asserts the helper's
 * verdict + diagnostic.
 *
 * Covers the rules from `speculative_compat.cpp`:
 *   - Null model handles
 *   - Recurrent target gate (Mamba/RWKV/hybrid)
 *   - Vocab type parity
 *   - BOS-add behavior + BOS token id parity
 *   - EOS-add behavior + EOS token id parity
 *   - Vocab size delta tolerance
 *   - Token text prefix equality
 *
 * @version 2.1.11
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/speculative_compat.h>

#include <llama.h>

#include <string>
#include <unordered_map>

namespace mock_llama {

/**
 * @brief Per-model mock attributes used by the link-time overrides.
 *
 * Tests configure these maps keyed by model pointer; the override
 * functions look up the pointer and return the configured values.
 * @utility
 * @version 2.1.11
 */
struct VocabAttrs {
    int vocab_type = 0;
    int n_tokens = 32000;
    bool add_bos = true;
    int bos = 1;
    bool add_eos = true;
    int eos = 2;
    std::unordered_map<int, std::string> token_text;
};

static std::unordered_map<const llama_model*, bool> recurrent_map;
static std::unordered_map<
    const llama_model*, const llama_vocab*> vocab_map;
static std::unordered_map<const llama_vocab*, VocabAttrs> attrs_map;

/**
 * @brief Reset all mock state between tests.
 * @utility
 * @version 2.1.11
 */
void reset() {
    recurrent_map.clear();
    vocab_map.clear();
    attrs_map.clear();
}

/**
 * @brief Register a target/draft model with synthetic vocab attrs.
 * @param model Sentinel model pointer.
 * @param vocab Sentinel vocab pointer.
 * @param attrs Vocab attributes.
 * @param recurrent Mark the model as recurrent (default false).
 * @utility
 * @version 2.1.11
 */
void register_model(
    const llama_model* model,
    const llama_vocab* vocab,
    VocabAttrs attrs,
    bool recurrent = false) {
    recurrent_map[model] = recurrent;
    vocab_map[model] = vocab;
    attrs_map[vocab] = std::move(attrs);
}

} // namespace mock_llama

extern "C" {

bool llama_model_is_recurrent(const struct llama_model* model) {
    auto it = mock_llama::recurrent_map.find(model);
    return it != mock_llama::recurrent_map.end() && it->second;
}

const struct llama_vocab* llama_model_get_vocab(
    const struct llama_model* model) {
    auto it = mock_llama::vocab_map.find(model);
    return it == mock_llama::vocab_map.end() ? nullptr : it->second;
}

enum llama_vocab_type llama_vocab_type(const struct llama_vocab* vocab) {
    auto it = mock_llama::attrs_map.find(vocab);
    return static_cast<enum llama_vocab_type>(
        it == mock_llama::attrs_map.end() ? 0 : it->second.vocab_type);
}

int32_t llama_vocab_n_tokens(const struct llama_vocab* vocab) {
    auto it = mock_llama::attrs_map.find(vocab);
    return it == mock_llama::attrs_map.end() ? 0 : it->second.n_tokens;
}

bool llama_vocab_get_add_bos(const struct llama_vocab* vocab) {
    auto it = mock_llama::attrs_map.find(vocab);
    return it != mock_llama::attrs_map.end() && it->second.add_bos;
}

bool llama_vocab_get_add_eos(const struct llama_vocab* vocab) {
    auto it = mock_llama::attrs_map.find(vocab);
    return it != mock_llama::attrs_map.end() && it->second.add_eos;
}

llama_token llama_vocab_bos(const struct llama_vocab* vocab) {
    auto it = mock_llama::attrs_map.find(vocab);
    return it == mock_llama::attrs_map.end() ? 0 : it->second.bos;
}

llama_token llama_vocab_eos(const struct llama_vocab* vocab) {
    auto it = mock_llama::attrs_map.find(vocab);
    return it == mock_llama::attrs_map.end() ? 0 : it->second.eos;
}

const char* llama_vocab_get_text(
    const struct llama_vocab* vocab, llama_token token) {
    auto it = mock_llama::attrs_map.find(vocab);
    if (it == mock_llama::attrs_map.end()) { return ""; }
    auto t_it = it->second.token_text.find(static_cast<int>(token));
    return (t_it == it->second.token_text.end())
               ? "<default>"
               : t_it->second.c_str();
}

} // extern "C"

// Sentinel handles — only their addresses matter for the mock map.
namespace {
int target_handle;
int draft_handle;
int target_vocab_handle;
int draft_vocab_handle;
const llama_model* TARGET =
    reinterpret_cast<const llama_model*>(&target_handle);
const llama_model* DRAFT =
    reinterpret_cast<const llama_model*>(&draft_handle);
const llama_vocab* VT =
    reinterpret_cast<const llama_vocab*>(&target_vocab_handle);
const llama_vocab* VD =
    reinterpret_cast<const llama_vocab*>(&draft_vocab_handle);

mock_llama::VocabAttrs default_attrs() {
    mock_llama::VocabAttrs a;
    a.vocab_type = 1;
    a.n_tokens = 32000;
    a.add_bos = true;
    a.bos = 1;
    a.add_eos = true;
    a.eos = 2;
    // Tokens 5..199 → "tok_N" — identical between target and draft
    // unless overridden by a test case.
    for (int i = 5; i < 200; ++i) {
        a.token_text[i] = "tok_" + std::to_string(i);
    }
    return a;
}
} // anonymous namespace

TEST_CASE("speculative compat: matching pair returns compatible",
          "[speculative][compat]") {
    mock_llama::reset();
    mock_llama::register_model(TARGET, VT, default_attrs());
    mock_llama::register_model(DRAFT, VD, default_attrs());
    auto r = entropic::speculative::check_compat(TARGET, DRAFT);
    REQUIRE(r.compatible);
    REQUIRE(r.diagnostic.empty());
}

TEST_CASE("speculative compat: null handles rejected",
          "[speculative][compat]") {
    mock_llama::reset();
    auto r1 = entropic::speculative::check_compat(nullptr, DRAFT);
    REQUIRE_FALSE(r1.compatible);
    REQUIRE(r1.diagnostic.find("null model") != std::string::npos);

    auto r2 = entropic::speculative::check_compat(TARGET, nullptr);
    REQUIRE_FALSE(r2.compatible);
    REQUIRE(r2.diagnostic.find("null model") != std::string::npos);
}

TEST_CASE("speculative compat: recurrent target is rejected",
          "[speculative][compat][recurrent]") {
    mock_llama::reset();
    mock_llama::register_model(
        TARGET, VT, default_attrs(), /*recurrent=*/true);
    mock_llama::register_model(DRAFT, VD, default_attrs());
    auto r = entropic::speculative::check_compat(TARGET, DRAFT);
    REQUIRE_FALSE(r.compatible);
    REQUIRE(r.diagnostic.find("recurrent") != std::string::npos);
}

TEST_CASE("speculative compat: vocab type mismatch rejected",
          "[speculative][compat]") {
    mock_llama::reset();
    auto a = default_attrs();
    auto b = default_attrs();
    b.vocab_type = 2;
    mock_llama::register_model(TARGET, VT, a);
    mock_llama::register_model(DRAFT, VD, b);
    auto r = entropic::speculative::check_compat(TARGET, DRAFT);
    REQUIRE_FALSE(r.compatible);
    REQUIRE(r.diagnostic.find("vocab type") != std::string::npos);
}

TEST_CASE("speculative compat: BOS mismatch rejected",
          "[speculative][compat]") {
    mock_llama::reset();
    auto a = default_attrs();
    auto b = default_attrs();
    b.bos = 99;
    mock_llama::register_model(TARGET, VT, a);
    mock_llama::register_model(DRAFT, VD, b);
    auto r = entropic::speculative::check_compat(TARGET, DRAFT);
    REQUIRE_FALSE(r.compatible);
    REQUIRE(r.diagnostic.find("BOS") != std::string::npos);
}

TEST_CASE("speculative compat: BOS-add behavior mismatch rejected",
          "[speculative][compat]") {
    mock_llama::reset();
    auto a = default_attrs();
    auto b = default_attrs();
    b.add_bos = false;
    mock_llama::register_model(TARGET, VT, a);
    mock_llama::register_model(DRAFT, VD, b);
    auto r = entropic::speculative::check_compat(TARGET, DRAFT);
    REQUIRE_FALSE(r.compatible);
    REQUIRE(r.diagnostic.find("BOS") != std::string::npos);
}

TEST_CASE("speculative compat: EOS mismatch rejected",
          "[speculative][compat]") {
    mock_llama::reset();
    auto a = default_attrs();
    auto b = default_attrs();
    b.eos = 99;
    mock_llama::register_model(TARGET, VT, a);
    mock_llama::register_model(DRAFT, VD, b);
    auto r = entropic::speculative::check_compat(TARGET, DRAFT);
    REQUIRE_FALSE(r.compatible);
    REQUIRE(r.diagnostic.find("EOS") != std::string::npos);
}

TEST_CASE("speculative compat: vocab size delta within tolerance",
          "[speculative][compat]") {
    mock_llama::reset();
    auto a = default_attrs();
    auto b = default_attrs();
    b.n_tokens = 32000 + 64;   // diff = 64, threshold = 128
    mock_llama::register_model(TARGET, VT, a);
    mock_llama::register_model(DRAFT, VD, b);
    auto r = entropic::speculative::check_compat(TARGET, DRAFT);
    REQUIRE(r.compatible);
}

TEST_CASE("speculative compat: vocab size delta over tolerance rejected",
          "[speculative][compat]") {
    mock_llama::reset();
    auto a = default_attrs();
    auto b = default_attrs();
    b.n_tokens = 32000 + 256;  // diff = 256, threshold = 128
    mock_llama::register_model(TARGET, VT, a);
    mock_llama::register_model(DRAFT, VD, b);
    auto r = entropic::speculative::check_compat(TARGET, DRAFT);
    REQUIRE_FALSE(r.compatible);
    REQUIRE(r.diagnostic.find("vocab size") != std::string::npos);
}

TEST_CASE("speculative compat: token text mismatch rejected",
          "[speculative][compat]") {
    mock_llama::reset();
    auto a = default_attrs();
    auto b = default_attrs();
    b.token_text[42] = "DIFFERENT";
    mock_llama::register_model(TARGET, VT, a);
    mock_llama::register_model(DRAFT, VD, b);
    auto r = entropic::speculative::check_compat(TARGET, DRAFT);
    REQUIRE_FALSE(r.compatible);
    REQUIRE(r.diagnostic.find("token text") != std::string::npos);
    REQUIRE(r.diagnostic.find("42") != std::string::npos);
}

TEST_CASE("speculative compat: token text in skipped range ignored",
          "[speculative][compat]") {
    mock_llama::reset();
    auto a = default_attrs();
    auto b = default_attrs();
    // Token id 3 is below kSpecVocabCheckStartTokenId (5) so a
    // mismatch here should NOT trigger rejection — matches the
    // upstream convention of skipping the BOS/EOS/UNK/PAD region.
    a.token_text[3] = "<pad>";
    b.token_text[3] = "<different-pad>";
    mock_llama::register_model(TARGET, VT, a);
    mock_llama::register_model(DRAFT, VD, b);
    auto r = entropic::speculative::check_compat(TARGET, DRAFT);
    REQUIRE(r.compatible);
}
