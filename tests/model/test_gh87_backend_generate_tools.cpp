// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh87_backend_generate_tools.cpp
 * @brief gh#87 Increment-3a gate: generate() routes through common_chat.
 *
 * Increment 2 proved render_with_tools + parse_response via the raw
 * complete() path. This proves the SAME common_chat path is reachable from
 * the standard generate() entry point — i.e. the internal render_prompt seam
 * routes generate() to render_with_tools when tools are staged.
 *
 * Tests the COMBINED call site (not the methods in isolation), per the
 * "test the combined call path" lesson: set_active_tools → generate →
 * has_common_chat_params() → parse_response. This is exactly the sequence
 * the orchestrator will run at the Increment-3b live cutover.
 *
 * generate() in 3a still returns raw content (the backend does not
 * self-parse); the orchestrator drives parse_response. SKIPs if the a4b
 * GGUF is absent.
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

TEST_CASE("gh#87 Increment-3a: generate() routes through common_chat when "
          "tools are staged",
          "[gh87][model][e2e]") {
    auto path = gemma4_a4b_path();
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("gemma4 a4b GGUF not present at " + path.string());
    }

    entropic::LlamaCppBackend backend;
    auto cfg = make_a4b_config(path);
    REQUIRE(backend.load(cfg));
    REQUIRE(backend.activate());

    // Tool-less render first: the seam must NOT capture params.
    std::vector<entropic::Message> probe;
    {
        entropic::Message u;
        u.role = "user";
        u.content = "hi";
        probe.push_back(u);
    }
    entropic::GenerationParams probe_params;
    probe_params.max_tokens = 1;
    probe_params.temperature = 0.0f;
    probe_params.enable_thinking = false;
    (void)backend.generate(probe, probe_params);
    bool captured_without_tools = backend.has_common_chat_params();

    // Now stage tools and generate through the standard entry point.
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
    params.max_tokens = 400;
    params.temperature = 0.0f;
    params.enable_thinking = true;

    auto gen = backend.generate(msgs, params);
    bool captured_with_tools = backend.has_common_chat_params();
    std::string raw = gen.content;

    // Orchestrator-style parse (3a: the backend does not self-parse).
    auto parsed = backend.parse_response(raw);
    size_t n_calls = parsed.tool_calls.size();
    entropic::ToolCall tc;
    if (n_calls >= 1) { tc = parsed.tool_calls[0]; }

    backend.deactivate();
    backend.unload();

    INFO("raw model output: " << raw);
    INFO("captured_without_tools=" << captured_without_tools
         << " captured_with_tools=" << captured_with_tools
         << " parsed tool_calls=" << n_calls);

    // The render seam: tool-less render leaves no captured params; the
    // tools render captures them (so the orchestrator routes to parse_response).
    CHECK_FALSE(captured_without_tools);
    REQUIRE(captured_with_tools);
    REQUIRE(n_calls >= 1);
    CHECK(tc.name == "read_file");
    CHECK(tc.arguments.count("path") == 1);
    CHECK(tc.arguments["path"].find("/etc/hostname") != std::string::npos);
}
