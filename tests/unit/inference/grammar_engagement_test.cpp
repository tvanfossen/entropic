// SPDX-License-Identifier: Apache-2.0
/**
 * @file grammar_engagement_test.cpp
 * @brief CPU-tier proof that a non-empty GenerationParams.grammar ACTUALLY
 *        engages a grammar constraint in the production sampler chain
 *        (gh#95, audit Pattern C). Closes the fail-open whereby grammar
 *        CONSTRAINT was only ever proven in GPU model tests that SKIP on CI,
 *        while add_grammar_sampler() logs UNCONSTRAINED and proceeds.
 *
 * Drives the REAL LlamaCppSamplerFactory against a vocab-only GGUF (no
 * weights, no llama_context needed) and asserts the grammar stage is present
 * in the chain AND functionally masks every grammar-illegal token. RED-first:
 * an empty grammar must leave the chain with NO grammar stage and mask nothing.
 *
 * @version 2.8.0 (gh#95)
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include "../../../src/inference/llama_cpp_sampler.h"

#include <llama.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Resolve a vocab-only GGUF relative to the source tree (ENTROPIC_SOURCE_DIR
// injected via target_compile_definitions). Mirrors vocab_only_smoke_test.cpp.
std::filesystem::path find_vocab_gguf(const std::string& name) {
#ifdef ENTROPIC_SOURCE_DIR
    return std::filesystem::path(ENTROPIC_SOURCE_DIR)
        / "extern" / "llama.cpp" / "models" / name;
#else
    return std::filesystem::path("extern/llama.cpp/models") / name;
#endif
}

// RAII vocab-only model (weights skipped; vocab borrowed from the model).
struct VocabOnlyModel {
    llama_model* model = nullptr;
    const llama_vocab* vocab = nullptr;
    explicit VocabOnlyModel(const std::filesystem::path& path) {
        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;
        mp.vocab_only   = true;
        mp.load_mode    = LLAMA_LOAD_MODE_MMAP;  // b11009: was use_mmap/use_mlock
        model = llama_model_load_from_file(path.c_str(), mp);
        if (model != nullptr) { vocab = llama_model_get_vocab(model); }
    }
    ~VocabOnlyModel() { if (model) { llama_model_free(model); } }
    VocabOnlyModel(const VocabOnlyModel&) = delete;
    VocabOnlyModel& operator=(const VocabOnlyModel&) = delete;
    bool valid() const { return model != nullptr && vocab != nullptr; }
};

// Does the built chain contain a stage named "grammar"? (llama.cpp names the
// grammar sampler "grammar".) Robust to the other default stages
// (temp/top_k/top_p/penalties/dist) which the default GenerationParams also
// activates, so we match by NAME, never by count.
bool chain_has_grammar_stage(llama_sampler* chain) {
    const int n = llama_sampler_chain_n(chain);
    for (int i = 0; i < n; ++i) {
        llama_sampler* s = llama_sampler_chain_get(chain, i);
        if (s != nullptr && std::string(llama_sampler_name(s)) == "grammar") {
            return true;
        }
    }
    return false;
}

// Build a uniform candidate array over the whole vocab, run the grammar
// stage's apply in isolation, and report how many tokens survived
// (logit != -inf). Isolating the grammar stage keeps temp/top_k/top_p from
// muddying the mask.
struct ApplyResult { int survivors = 0; int total = 0; };

ApplyResult apply_grammar_only(llama_sampler* chain, const llama_vocab* vocab) {
    llama_sampler* g = nullptr;
    const int n = llama_sampler_chain_n(chain);
    for (int i = 0; i < n; ++i) {
        llama_sampler* s = llama_sampler_chain_get(chain, i);
        if (s != nullptr && std::string(llama_sampler_name(s)) == "grammar") {
            g = s; break;
        }
    }
    ApplyResult r;
    const int n_vocab = llama_vocab_n_tokens(vocab);
    r.total = n_vocab;
    std::vector<llama_token_data> cand;
    cand.reserve(n_vocab);
    for (int id = 0; id < n_vocab; ++id) {
        cand.push_back({static_cast<llama_token>(id), 0.0f, 0.0f});
    }
    llama_token_data_array arr{cand.data(), cand.size(), -1, false};
    if (g != nullptr) { llama_sampler_apply(g, &arr); }
    for (const auto& c : cand) {
        if (!std::isinf(c.logit) || c.logit > 0.0f) { ++r.survivors; }
    }
    return r;
}

} // namespace

// ── GREEN: a non-empty grammar engages a real constraint ────────────
SCENARIO("A non-empty GBNF grammar engages a grammar stage that masks "
         "illegal tokens (gh#95 fail-open closed at CPU tier)",
         "[gh95][grammar][inference][cpu][engagement]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-bpe.gguf");
    if (!std::filesystem::exists(path)) {
        WARN("vocab GGUF missing at " + path.string()
             + " — skipping grammar engagement test");
        return;
    }
    VocabOnlyModel om(path);
    REQUIRE(om.valid());

    GIVEN("the REAL LlamaCppSamplerFactory bound to a real vocab "
          "(null context — grammar attach + apply never touch ctx)") {
        entropic::LlamaCppSamplerFactory factory(/*ctx=*/nullptr, om.vocab);

        WHEN("create() runs with a trivial constraining grammar root::=\"x\"") {
            entropic::GenerationParams p;          // defaults are fine
            p.grammar = "root ::= \"x\"";
            auto sampler = factory.create(p);
            REQUIRE(sampler != nullptr);

            // Down-cast to reach native_chain() — production accessor that
            // exists precisely for tests asserting production wiring really
            // produces a non-null chain.
            auto* concrete =
                dynamic_cast<entropic::LlamaCppSampler*>(sampler.get());
            REQUIRE(concrete != nullptr);
            llama_sampler* chain = concrete->native_chain();
            REQUIRE(chain != nullptr);

            THEN("the chain CONTAINS a grammar stage (not fail-open)") {
                REQUIRE(chain_has_grammar_stage(chain));
            }
            AND_THEN("the grammar functionally masks illegal tokens: "
                     "at least one survives, but NOT the whole vocab") {
                auto res = apply_grammar_only(chain, om.vocab);
                REQUIRE(res.total > 1);
                REQUIRE(res.survivors >= 1);          // 'x'-leading token(s)
                REQUIRE(res.survivors < res.total);    // most are masked off
            }
        }
    }
}

// ── RED-first control: empty grammar ⇒ NO stage, NO masking ──────────
SCENARIO("An empty grammar leaves the chain UNCONSTRAINED — the control "
         "that turns RED if grammar gating is bypassed (gh#95)",
         "[gh95][grammar][inference][cpu][engagement][failure-mode]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-bpe.gguf");
    if (!std::filesystem::exists(path)) {
        WARN("vocab GGUF missing — skipping empty-grammar control");
        return;
    }
    VocabOnlyModel om(path);
    REQUIRE(om.valid());

    GIVEN("the REAL factory and an empty grammar") {
        entropic::LlamaCppSamplerFactory factory(nullptr, om.vocab);
        entropic::GenerationParams p;   // p.grammar == "" by default

        WHEN("create() runs") {
            auto sampler = factory.create(p);
            auto* concrete =
                dynamic_cast<entropic::LlamaCppSampler*>(sampler.get());
            REQUIRE(concrete != nullptr);
            llama_sampler* chain = concrete->native_chain();

            THEN("there is NO grammar stage in the chain") {
                REQUIRE_FALSE(chain_has_grammar_stage(chain));
            }
            AND_THEN("nothing is masked — every token survives") {
                auto res = apply_grammar_only(chain, om.vocab);
                REQUIRE(res.survivors == res.total);
            }
        }
    }
}

// ── v2.14.6: the tool-call grammar reaches PLAIN decode ──────────────
//
// gh#134 applied the render's tool-call grammar only on the common_sampler
// path (speculative / MTP). Plain decode built its chain from params.grammar
// alone, so require_tool_call and the forced retry after a nudge decoded
// unconstrained (measured: E4B wrote {"action":"complete",...} as text on
// every forced retry). These drive the real factory's tool-grammar overload.

namespace {

/// Single token id for @p text, or -1 when it is not exactly one token.
llama_token one_token(const llama_vocab* vocab, const std::string& text) {
    llama_token t[4];
    int n = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                           t, 4, false, true);
    return n == 1 ? t[0] : -1;
}

/// Does token @p id survive the chain's grammar stage?
bool survives(llama_sampler* chain, const llama_vocab* vocab, llama_token id) {
    llama_sampler* g = nullptr;
    for (int i = 0; i < llama_sampler_chain_n(chain); ++i) {
        auto* s = llama_sampler_chain_get(chain, i);
        if (std::string(llama_sampler_name(s)) == "grammar") { g = s; }
    }
    std::vector<llama_token_data> cand;
    for (int i = 0; i < llama_vocab_n_tokens(vocab); ++i) {
        cand.push_back({static_cast<llama_token>(i), 0.0f, 0.0f});
    }
    llama_token_data_array arr{cand.data(), cand.size(), -1, false};
    if (g != nullptr) { llama_sampler_apply(g, &arr); }
    return !std::isinf(cand[static_cast<size_t>(id)].logit);
}

/// Chain of a Sampler built by the production factory.
llama_sampler* chain_of(const std::unique_ptr<entropic::Sampler>& s) {
    auto* c = dynamic_cast<entropic::LlamaCppSampler*>(s.get());
    return c != nullptr ? c->native_chain() : nullptr;
}

const char* kToolGbnf = "root ::= \"(\" \"x\"";

} // namespace

SCENARIO("v2.14.6: a non-lazy tool-call grammar is applied on plain decode, "
         "prefilled with the generation prompt",
         "[2.14.6][grammar][inference][cpu][engagement]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-bpe.gguf");
    REQUIRE(std::filesystem::exists(path));
    VocabOnlyModel om(path);
    REQUIRE(om.valid());
    entropic::LlamaCppSamplerFactory factory(nullptr, om.vocab);
    const llama_token open = one_token(om.vocab, "(");
    const llama_token x = one_token(om.vocab, "x");
    REQUIRE(open >= 0);
    REQUIRE(x >= 0);
    entropic::GenerationParams p;

    WHEN("the render prefilled the grammar's opening ( into the prompt") {
        auto s = factory.create(p, {kToolGbnf, false, "("});
        REQUIRE(s != nullptr);
        llama_sampler* chain = chain_of(s);
        THEN("the grammar stage is present and continues after the prefill") {
            REQUIRE(chain_has_grammar_stage(chain));
            CHECK(survives(chain, om.vocab, x));
            CHECK_FALSE(survives(chain, om.vocab, open));
        }
    }
    WHEN("there is no prefill") {
        auto s = factory.create(p, {kToolGbnf, false, ""});
        REQUIRE(s != nullptr);
        THEN("the grammar starts at its root") {
            CHECK(survives(chain_of(s), om.vocab, open));
            CHECK_FALSE(survives(chain_of(s), om.vocab, x));
        }
    }
    WHEN("the grammar rejects its own prefill") {
        auto s = factory.create(p, {kToolGbnf, false, "zz"});
        THEN("no sampler is built: never decode unconstrained") {
            CHECK(s == nullptr);
        }
    }
    WHEN("the tool grammar is lazy (no trigger patterns are captured)") {
        auto s = factory.create(p, {kToolGbnf, true, "("});
        REQUIRE(s != nullptr);
        THEN("it is not applied") {
            CHECK_FALSE(chain_has_grammar_stage(chain_of(s)));
        }
    }
    WHEN("a request grammar is also set") {
        p.grammar = "root ::= \"x\"";
        auto s = factory.create(p, {kToolGbnf, false, ""});
        REQUIRE(s != nullptr);
        THEN("the request grammar wins") {
            CHECK(survives(chain_of(s), om.vocab, x));
            CHECK_FALSE(survives(chain_of(s), om.vocab, open));
        }
    }
}
