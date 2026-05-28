// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh87_backend_common_chat.cpp
 * @brief gh#87 Increment-2 gate: LlamaCppBackend common_chat tool path.
 *
 * Increment 1 proved the raw llama.cpp `common_chat` triple works on real
 * gemma4 tokens. This drives the SAME triple through entropic's backend
 * surface — the additive gh#87 methods:
 *
 *   1. backend.set_active_tools(mcp_json)   — stage MCP tool defs
 *   2. backend.render_with_tools(msgs)      — jinja render WITH inputs.tools,
 *                                             capturing the parse params
 *   3. backend.complete(prompt)             — raw decode (no re-template)
 *   4. backend.parse_response(raw)          — common_chat_parse via the
 *                                             captured params (loads the arena)
 *
 * Asserts the gemma4-native `read_file` call round-trips to entropic::ToolCall.
 * The legacy apply_chat_template / adapter path is untouched (Increment 3
 * does the production cutover). SKIPs if the a4b GGUF is absent.
 *
 * @version 2.7.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include <entropic/types/generation_result.h>
#include <entropic/types/message.h>

#include "../../src/inference/llama_cpp_backend.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path gemma4_a4b_path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') { return {}; }
    return std::filesystem::path(home) / ".entropic" / "models"
         / "gemma-4-26B-A4B-it-UD-IQ4_XS.gguf";
}

entropic::ModelConfig make_a4b_config(const std::filesystem::path& gguf) {
    entropic::ModelConfig cfg;
    cfg.path = gguf;
    cfg.adapter = "gemma4";
    cfg.context_length = 4096;
    cfg.gpu_layers = 20;     // 26B partial CPU offload on an 11GB GPU
    cfg.keep_warm = false;
    cfg.use_mlock = false;
    cfg.flash_attn = false;
    cfg.n_batch = 512;
    cfg.cache_type_k = "f16";
    cfg.cache_type_v = "f16";
    return cfg;
}

// MCP list_tools() shape: array of {name, description, inputSchema}.
const char* kReadFileToolJson = R"([{
  "name": "read_file",
  "description": "Read a file from disk.",
  "inputSchema": {
    "type": "object",
    "properties": {"path": {"type": "string"}},
    "required": ["path"]
  }
}])";

}  // namespace

TEST_CASE("gh#87 Increment-2: backend render_with_tools → parse_response "
          "extracts a gemma4 tool call",
          "[gh87][model][e2e]") {
    auto path = gemma4_a4b_path();
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("gemma4 a4b GGUF not present at " + path.string());
    }

    entropic::LlamaCppBackend backend;
    auto cfg = make_a4b_config(path);
    REQUIRE(backend.load(cfg));
    REQUIRE(backend.activate());

    backend.set_active_tools(kReadFileToolJson);

    std::vector<entropic::Message> msgs;
    {
        entropic::Message u;
        u.role = "user";
        u.content = "Use the read_file tool to read /etc/hostname. "
                    "Emit only the tool call.";
        msgs.push_back(u);
    }

    entropic::GenerationParams params;
    params.max_tokens = 400;       // room for the closing <tool_call|>
    params.temperature = 0.0f;     // deterministic
    params.enable_thinking = true; // gemma deliberates then calls — fine

    std::string prompt = backend.render_with_tools(msgs, params);
    REQUIRE_FALSE(prompt.empty());

    // Raw completion: no chat template re-applied, so we decode exactly
    // what render_with_tools produced.
    auto gen = backend.complete(prompt, params);
    std::string raw = gen.content;

    auto parsed = backend.parse_response(raw);

    // Capture outcome before teardown.
    size_t n_calls = parsed.tool_calls.size();
    entropic::ToolCall tc;
    if (n_calls >= 1) { tc = parsed.tool_calls[0]; }

    backend.deactivate();
    backend.unload();

    INFO("raw model output: " << raw);
    INFO("parsed tool_calls=" << n_calls
         << " content=[" << parsed.content << "]"
         << " reasoning=[" << parsed.reasoning_content << "]");
    REQUIRE(n_calls >= 1);
    CHECK(tc.name == "read_file");
    CHECK(tc.arguments.count("path") == 1);
    CHECK(tc.arguments["path"].find("/etc/hostname") != std::string::npos);
}
