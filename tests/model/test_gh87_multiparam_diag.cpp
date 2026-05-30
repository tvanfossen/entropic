// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh87_multiparam_diag.cpp
 * @brief gh#87 diagnostic: does common_chat parse a MULTI-parameter tool call?
 *
 * e2 (1-param read_file) parses via common_chat; e8 (2-param write_file)
 * does not — same emitted format. This isolates whether common_chat's
 * parser handles multi-parameter tool calls for the default model, and
 * dumps the rendered prompt (what format the model is INSTRUCTED to use)
 * plus the raw emission and parse result. Diagnostic only — not a gate.
 *
 * @version 2.7.0
 */

#include "gh87_verify_helpers.h"

#include <cstdio>

// gemma4-e4b (dedicated PEG_GEMMA4) parses multi-param via common_chat —
// confirmed. This diag now stays on gemma4 as a permanent multi-param gate
// for the common_chat path (LOCAL model, no HDD dependency).
TEST_CASE("gh#87 diag: multi-parameter tool call — gemma4 (dedicated PEG)",
          "[gh87][model][diag]") {
    auto path = gh87verify::model_path("gemma-4-E4B-it-Q8_0.gguf");
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("gemma4-e4b GGUF not present at " + path.string());
    }

    entropic::LlamaCppBackend backend;
    entropic::ModelConfig cfg;
    cfg.path = path;
    cfg.adapter = "gemma4";
    cfg.context_length = 4096;
    cfg.gpu_layers = 99;
    cfg.keep_warm = false;
    cfg.use_mlock = false;
    cfg.flash_attn = false;
    cfg.n_batch = 512;
    cfg.cache_type_k = "f16";
    cfg.cache_type_v = "f16";
    REQUIRE(backend.load(cfg));
    REQUIRE(backend.activate());

    // Two-parameter tool (the e8 write_file shape).
    backend.set_active_tools(
        R"([{"name":"write_file",)"
        R"("description":"Write content to a file on disk.",)"
        R"("inputSchema":{"type":"object","properties":{"path":)"
        R"({"type":"string"},"content":{"type":"string"}},)"
        R"("required":["path","content"]}}])");

    std::vector<entropic::Message> msgs;
    {
        entropic::Message u;
        u.role = "user";
        u.content = "Use the write_file tool to write the text hello "
                    "to the file out.txt. Respond with only the tool call.";
        msgs.push_back(u);
    }
    entropic::GenerationParams params;
    params.max_tokens = 512;
    params.temperature = 0.0f;
    params.enable_thinking = true;

    std::string prompt = backend.render_with_tools(msgs, params);
    auto gen = backend.generate(msgs, params);
    auto parsed = backend.parse_response(gen.content);

    size_t n = parsed.tool_calls.size();
    std::string nm = n >= 1 ? parsed.tool_calls[0].name : "";
    size_t nargs = n >= 1 ? parsed.tool_calls[0].arguments.size() : 0;

    backend.deactivate();
    backend.unload();

    // Dump everything to stderr for diagnosis (visible with 2>&1).
    fprintf(stderr, "\n===== RENDERED PROMPT (tool-format instruction) =====\n%s\n",
            prompt.c_str());
    fprintf(stderr, "===== RAW EMISSION =====\n%s\n", gen.content.c_str());
    fprintf(stderr, "===== PARSE: %zu call(s), name=[%s], %zu arg(s) =====\n",
            n, nm.c_str(), nargs);

    // Not strict — this is a diagnostic. Record the outcome.
    CHECK(n >= 1);
    if (n >= 1) { CHECK(nargs >= 2); }
}
