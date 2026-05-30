// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh87_common_chat_e2e.cpp
 * @brief gh#87 increment-1 gate: real-flow common_chat render→generate→parse.
 *
 * The isolated-string spike proved `common_chat_parse` is coupled to the
 * render's generation_prompt and the model's actual token stream, so it
 * can't be validated on hand-typed input. This test closes that gap:
 * it loads gemma4-a4b (26B), renders WITH tools via `common_chat_templates_apply`,
 * runs a minimal greedy decode to get the model's REAL native emission,
 * and parses it back via `common_chat_parse` — asserting a tool call is
 * extracted and maps to entropic::ToolCall.
 *
 * This is the de-risk gate before refactoring entropic's production
 * tool-call path onto common_chat (gh#87). SKIPs if the GGUF is absent.
 *
 * @version 2.7.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/tool_call.h>

#include <llama.h>
#include <chat.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path gemma4_model_path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) { return {}; }
    // Use a4b (26B) for the architecture gate: every gemma4 GGUF on the
    // dev box is 4-bit (the "Q8_0" filenames are v2.4.0 substitution
    // symlinks to Q4 files), and the 2B-at-4bit emits quant-degraded
    // tool calls that would confound "does common_chat parse gemma's
    // native format". A 26B even at IQ4_XS formats reliably enough to
    // isolate the architecture from quant. CPU-offloaded on an 11GB GPU.
    return std::filesystem::path(home) / ".entropic" / "models"
         / "gemma-4-26B-A4B-it-UD-IQ4_XS.gguf";
}

std::vector<llama_token> tokenize(const llama_vocab* vocab,
                                  const std::string& text) {
    int n = -llama_tokenize(vocab, text.c_str(),
                            static_cast<int32_t>(text.size()),
                            nullptr, 0, /*add_special=*/true,
                            /*parse_special=*/true);
    std::vector<llama_token> out(static_cast<size_t>(n));
    int written = llama_tokenize(vocab, text.c_str(),
                                 static_cast<int32_t>(text.size()),
                                 out.data(), n, true, true);
    out.resize(static_cast<size_t>(written < 0 ? 0 : written));
    return out;
}

std::string piece(const llama_vocab* vocab, llama_token tok) {
    char buf[256];
    int n = llama_token_to_piece(vocab, tok, buf, sizeof(buf),
                                 /*lstrip=*/0, /*special=*/true);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

/// @brief Minimal greedy decode of a rendered prompt → emitted text.
std::string greedy_generate(llama_context* ctx, const llama_vocab* vocab,
                            const std::string& prompt, int max_new) {
    auto toks = tokenize(vocab, prompt);
    if (toks.empty()) { return {}; }
    llama_batch batch = llama_batch_get_one(toks.data(),
                                            static_cast<int32_t>(toks.size()));
    if (llama_decode(ctx, batch) != 0) { return {}; }

    llama_sampler* smpl = llama_sampler_init_greedy();
    std::string out;
    for (int i = 0; i < max_new; ++i) {
        llama_token tok = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, tok)) { break; }
        out += piece(vocab, tok);
        llama_batch one = llama_batch_get_one(&tok, 1);
        if (llama_decode(ctx, one) != 0) { break; }
    }
    llama_sampler_free(smpl);
    return out;
}

entropic::ToolCall to_entropic(const common_chat_tool_call& cc) {
    entropic::ToolCall tc;
    tc.name = cc.name;
    tc.arguments_json = cc.arguments;
    tc.id = cc.id;
    auto j = nlohmann::json::parse(cc.arguments, nullptr, false);
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            tc.arguments[it.key()] =
                it->is_string() ? it->get<std::string>() : it->dump();
        }
    }
    return tc;
}

}  // namespace

// Flat TEST_CASE (not SCENARIO): the model loads exactly once and is
// freed before any assertion, so a failing REQUIRE can't leak the 26B
// and OOM a re-entered section.
TEST_CASE("gh#87 gate: common_chat render→generate→parse extracts a gemma4 tool call",
          "[gh87][model][e2e]") {
    auto path = gemma4_model_path();
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("gemma4 a4b GGUF not present at " + path.string());
    }

    llama_backend_init();
    auto mparams = llama_model_default_params();
    mparams.n_gpu_layers = 20;  // a4b 26B: partial CPU offload on 11GB GPU
    llama_model* model = llama_model_load_from_file(path.c_str(), mparams);
    REQUIRE(model != nullptr);
    const llama_vocab* vocab = llama_model_get_vocab(model);

    auto tmpls = common_chat_templates_init(model, "");
    REQUIRE(tmpls);

    common_chat_templates_inputs inputs;
    inputs.use_jinja = true;
    inputs.add_generation_prompt = true;
    inputs.tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;  // force a call
    {
        common_chat_msg u;
        u.role = "user";
        u.content = "Use the read_file tool to read /etc/hostname. "
                    "Emit only the tool call.";
        inputs.messages.push_back(u);
    }
    {
        common_chat_tool t;
        t.name = "read_file";
        t.description = "Read a file from disk.";
        t.parameters =
            R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})";
        inputs.tools.push_back(t);
    }

    common_chat_params rendered = common_chat_templates_apply(tmpls.get(), inputs);

    auto cparams = llama_context_default_params();
    cparams.n_ctx = 4096;
    cparams.n_batch = 4096;
    llama_context* ctx = llama_init_from_model(model, cparams);
    REQUIRE(ctx != nullptr);

    // Generous budget so the closing <tool_call|> the PEG rule requires
    // is actually emitted (200 truncated it mid-call).
    std::string raw = greedy_generate(ctx, vocab, rendered.prompt, 400);

    // The parser_params ctor copies only `format` + `generation_prompt`,
    // NOT the saved PEG arena — without this load(), common_chat_parse
    // silently falls back to a pure-content parser and extracts zero
    // tool calls. Production (gh#87 backend) must do the same.
    common_chat_parser_params pp(rendered);
    pp.parser.load(rendered.parser);
    common_chat_msg msg = common_chat_parse(raw, false, pp);

    // Capture before freeing so assertions never touch llama state.
    size_t n_calls = msg.tool_calls.size();
    entropic::ToolCall tc;
    if (n_calls >= 1) { tc = to_entropic(msg.tool_calls[0]); }

    llama_free(ctx);
    llama_model_free(model);

    INFO("raw model output: " << raw);
    INFO("format=" << static_cast<int>(rendered.format)
         << " parsed tool_calls=" << n_calls);
    REQUIRE(rendered.format == COMMON_CHAT_FORMAT_PEG_GEMMA4);
    REQUIRE(n_calls >= 1);
    CHECK(tc.name == "read_file");
    CHECK(tc.arguments.count("path") == 1);
    CHECK(tc.arguments["path"].find("/etc/hostname") != std::string::npos);
}
