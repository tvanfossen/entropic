// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v2146_gemma4_required_grammar.cpp
 * @brief The Gemma 4 tool_choice=REQUIRED grammar actually forces one call
 *        and then the end of the turn (v2.14.6).
 *
 * Symptom (GPU, gemma-4-E4B-it-qat-UD-Q4_K_XL, forced retry offering only
 * entropic.complete): the model wrote a call with plain `"` strings, did not
 * stop, and ran 4096 tokens of calls and prose; the parse found 0 calls. At
 * the pinned llama.cpp the REQUIRED root `start message* scan-to-toolcall
 * tool-call` accepts any text (each `until` can end mid-delimiter and the
 * loop restarts it), so it only masked EOG until a valid call appeared.
 * patch_gemma4_required_root mirrors upstream f072b10371.
 *
 * Vocab-only: the E4B GGUF is opened for its vocab and chat template, no
 * weights and no GPU. The render mirrors render_common_chat, and the grammar
 * runs through the production LlamaCppSamplerFactory with its prefill.
 *
 * @version 2.14.6
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include "gemma4_required_grammar.h"
#include "llama_cpp_sampler.h"

#include <chat.h>
#include <llama.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef MODEL_PATH
#define MODEL_PATH ENTROPIC_SOURCE_DIR  // when compiled into a unit target
#endif

namespace {

const char* kE4b = "gemma-4-E4B-it-qat-UD-Q4_K_XL.gguf";

/// A call the constrained model wrote on an earlier, healthy forced retry.
const char* kGoodBlock =
    "<|channel>thought\nThe task is done, so I call entropic.complete."
    "<channel|><|tool_call>call:entropic.complete{summary:<|\"|>Successfully "
    "updated notes.md to contain only the single line 'REWRITTEN BY ENG' as "
    "requested by the user.<|\"|>}<tool_call|>";

/// The minimal plain-quote call: not Gemma 4 tool syntax.
const char* kPlainQuoteCall =
    "<|tool_call>call:entropic.complete{summary:\"a\"}<tool_call|>";

/// Head of the 4096-token runaway, verbatim from the GPU log.
const char* kRunaway =
    "<|channel>thought\nThe user is asking me to say hello in one short "
    "sentence. I will call `entropic.complete` with a summary of the action "
    "taken.<channel|><|tool_call>call:entropic.complete{coverage_gap:false,"
    "gap_description:\"\",suggested_files:[],summary:\"Successfully greeted "
    "the user in one short sentence.\"}<tool_call|>Hello! I hope you have a "
    "wonderful day.";

/// RAII vocab-only model.
struct VocabOnly {
    llama_model* model = nullptr;
    const llama_vocab* vocab = nullptr;
    explicit VocabOnly(const std::filesystem::path& p) {
        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 0;
        mp.vocab_only = true;
        model = llama_model_load_from_file(p.c_str(), mp);
        if (model != nullptr) { vocab = llama_model_get_vocab(model); }
    }
    ~VocabOnly() { if (model != nullptr) { llama_model_free(model); } }
    VocabOnly(const VocabOnly&) = delete;
    VocabOnly& operator=(const VocabOnly&) = delete;
};

std::filesystem::path e4b_path() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : "")
        / ".entropic" / "models" / kE4b;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// The staged tool exactly as mcp_tools_to_common_chat builds it.
common_chat_tool complete_tool() {
    auto j = nlohmann::json::parse(slurp(
        std::filesystem::path(MODEL_PATH) / "data/tools/entropic/complete.json"));
    common_chat_tool t;
    t.name = "entropic.complete";
    t.description = j.value("description", "");
    t.parameters = j["inputSchema"].dump();
    return t;
}

/// The REQUIRED render, mirroring render_common_chat field for field.
common_chat_params render_required(llama_model* model) {
    auto tmpls = common_chat_templates_init(model, "");
    common_chat_templates_inputs in;
    common_chat_msg sys;
    sys.role = "system";
    sys.content = "You are a helpful assistant.";
    common_chat_msg usr;
    usr.role = "user";
    usr.content = "Say hello in one short sentence.";
    in.messages = {sys, usr};
    in.add_generation_prompt = true;
    in.use_jinja = true;
    in.enable_thinking = true;
    in.tools = {complete_tool()};
    in.tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    return common_chat_templates_apply(tmpls.get(), in);
}

std::vector<llama_token> tokenize(const llama_vocab* v, const std::string& s) {
    const auto len = static_cast<int32_t>(s.size());
    const int n = -llama_tokenize(v, s.c_str(), len, nullptr, 0, false, true);
    std::vector<llama_token> out(static_cast<size_t>(n));
    llama_tokenize(v, s.c_str(), len, out.data(), n, false, true);
    return out;
}

std::string piece(const llama_vocab* v, llama_token t) {
    char buf[256];
    const int n = llama_token_to_piece(v, t, buf, sizeof(buf), 0, true);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

/// A production sampler with the render's grammar, prefilled, plus its grammar stage.
struct GrammarUnderTest {
    std::unique_ptr<entropic::Sampler> sampler;
    llama_sampler* grammar = nullptr;
};

GrammarUnderTest make_grammar(const llama_vocab* v, const common_chat_params& r,
                              const std::string& gbnf) {
    entropic::LlamaCppSamplerFactory factory(nullptr, v);  // chain outlives it
    GrammarUnderTest g;
    g.sampler = factory.create(entropic::GenerationParams{},
        entropic::ToolCallGrammar{gbnf, false, r.generation_prompt});
    auto* concrete = dynamic_cast<entropic::LlamaCppSampler*>(g.sampler.get());
    llama_sampler* chain = concrete != nullptr ? concrete->native_chain() : nullptr;
    const int n = chain != nullptr ? llama_sampler_chain_n(chain) : 0;
    for (int i = 0; n > i; ++i) {
        auto* s = llama_sampler_chain_get(chain, i);
        if (std::string(llama_sampler_name(s)) == "grammar") { g.grammar = s; }
    }
    return g;
}

bool allows(llama_sampler* g, llama_token t) {
    llama_token_data d{t, 0.0f, 0.0f};
    llama_token_data_array arr{&d, 1, -1, false};
    llama_sampler_apply(g, &arr);
    return !std::isinf(d.logit);
}

/// Feed @p text; index of the first rejected token, or -1 when all accepted.
int first_rejected(llama_sampler* g, const std::vector<llama_token>& toks) {
    int rejected = -1;
    const int n = static_cast<int>(toks.size());
    for (int i = 0; n > i && rejected == -1; ++i) {
        if (allows(g, toks[static_cast<size_t>(i)])) {
            llama_sampler_accept(g, toks[static_cast<size_t>(i)]);
        } else {
            rejected = i;
        }
    }
    return rejected;
}

/// Tokens the grammar stage still allows, over the whole vocab.
std::vector<llama_token> allowed_set(llama_sampler* g, const llama_vocab* v) {
    const int n_vocab = llama_vocab_n_tokens(v);
    std::vector<llama_token_data> cand;
    cand.reserve(static_cast<size_t>(n_vocab));
    for (int i = 0; n_vocab > i; ++i) { cand.push_back({i, 0.0f, 0.0f}); }
    llama_token_data_array arr{cand.data(), cand.size(), -1, false};
    llama_sampler_apply(g, &arr);
    std::vector<llama_token> out;
    for (const auto& c : cand) {
        if (!std::isinf(c.logit)) { out.push_back(c.id); }
    }
    return out;
}

} // namespace

TEST_CASE("v2.14.6: the Gemma 4 REQUIRED grammar forces one call, then EOG",
          "[2.14.6][grammar][gemma4][vocab_only]")
{
    REQUIRE(std::filesystem::exists(e4b_path()));
    VocabOnly m(e4b_path());
    REQUIRE(m.vocab != nullptr);
    const auto r = render_required(m.model);
    REQUIRE(r.format == COMMON_CHAT_FORMAT_PEG_GEMMA4);
    REQUIRE_FALSE(r.grammar_lazy);
    const std::string gbnf = entropic::patch_gemma4_required_root(r.grammar);
    INFO("root line: " << entropic::gbnf_root_line(gbnf));

    SECTION("the good <|\"|> call is accepted, and after it only EOG is allowed") {
        auto g = make_grammar(m.vocab, r, gbnf);
        REQUIRE(g.grammar != nullptr);
        CHECK(first_rejected(g.grammar, tokenize(m.vocab, kGoodBlock)) == -1);
        const auto next = allowed_set(g.grammar, m.vocab);
        REQUIRE_FALSE(next.empty());
        int non_eog = 0;
        for (auto t : next) { non_eog += llama_vocab_is_eog(m.vocab, t) ? 0 : 1; }
        CHECK(non_eog == 0);
    }
    SECTION("a plain-quote call is rejected at the :\" token") {
        auto g = make_grammar(m.vocab, r, gbnf);
        REQUIRE(g.grammar != nullptr);
        const auto toks = tokenize(m.vocab, kPlainQuoteCall);
        const int at = first_rejected(g.grammar, toks);
        REQUIRE(at >= 0);
        CHECK(piece(m.vocab, toks[static_cast<size_t>(at)]).rfind(":\"", 0) == 0);
    }
    SECTION("the runaway text is rejected") {
        auto g = make_grammar(m.vocab, r, gbnf);
        REQUIRE(g.grammar != nullptr);
        CHECK(first_rejected(g.grammar, tokenize(m.vocab, kRunaway)) >= 0);
    }
}
