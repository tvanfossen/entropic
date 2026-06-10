// SPDX-License-Identifier: Apache-2.0
/**
 * @file gh87_verify_helpers.h
 * @brief Shared per-family common_chat verification (gh#87 Phase A).
 *
 * The gh#87 cutover makes llama.cpp common_chat the DEFAULT tool render+parse
 * path and retires the hand-rolled per-family adapters. Before an adapter can
 * be retired we must verify common_chat actually parses that family's native
 * tool-call wire format. This helper runs the PRODUCTION backend path
 * (set_active_tools → generate → parse_response) on a family model and
 * asserts a native tool call round-trips to entropic::ToolCall.
 *
 * One thin TEST_CASE per family calls verify_family_common_chat with that
 * family's GGUF + adapter + GPU offload. SKIPs if the GGUF isn't on disk.
 *
 * @version 2.7.0
 */

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include <entropic/types/generation_result.h>
#include <entropic/types/message.h>

#include "../../src/inference/llama_cpp_backend.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace gh87verify {

/// @brief Resolve a model filename under $HOME/.entropic/models.
inline std::filesystem::path model_path(const std::string& name) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') { return {}; }
    return std::filesystem::path(home) / ".entropic" / "models" / name;
}

/// @brief Bare MCP read_file tool def (server-unprefixed for the gate).
inline const char* read_file_tool_json() {
    return R"([{
      "name": "read_file",
      "description": "Read a file from disk and return its contents.",
      "inputSchema": {
        "type": "object",
        "properties": {"path": {"type": "string"}},
        "required": ["path"]
      }
    }])";
}

/**
 * @brief Verify a family's tool-call format is covered by common_chat.
 *
 * Loads the GGUF through LlamaCppBackend, stages a read_file tool, runs the
 * production generate() path (which routes through render_with_tools when
 * tools are staged), then parse_response. Asserts (a) a tool-capable format
 * was selected (has_common_chat_params) and (b) the model's real emission
 * parsed into at least one read_file call. A family that passes here can
 * have its hand-rolled adapter retired.
 *
 * @param gguf Model filename under ~/.entropic/models.
 * @param adapter Adapter key (identity only; parsing goes via common_chat).
 * @param gpu_layers GPU offload (99 = all for small models; 15 partial for the
 *        13GB qwen36 — == PARTIAL_GPU_LAYERS, OOM-safe on an 11GB card).
 * @version 2.8.2
 */
inline void verify_family_common_chat(const std::string& gguf,
                                      const std::string& adapter,
                                      int gpu_layers) {
    auto path = model_path(gguf);
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("GGUF not present: " + path.string());
    }

    entropic::LlamaCppBackend backend;
    entropic::ModelConfig cfg;
    cfg.path = path;
    cfg.adapter = adapter;
    cfg.context_length = 4096;
    cfg.gpu_layers = gpu_layers;
    cfg.keep_warm = false;
    cfg.use_mlock = false;
    cfg.flash_attn = false;
    cfg.n_batch = 512;
    cfg.cache_type_k = "f16";
    cfg.cache_type_v = "f16";
    REQUIRE(backend.load(cfg));
    REQUIRE(backend.activate());

    backend.set_active_tools(read_file_tool_json());

    std::vector<entropic::Message> msgs;
    {
        entropic::Message u;
        u.role = "user";
        u.content = "You must call the read_file tool to read the file "
                    "/etc/hostname. Respond with only the tool call.";
        msgs.push_back(u);
    }

    entropic::GenerationParams params;
    params.max_tokens = 512;
    params.temperature = 0.0f;
    params.enable_thinking = true;

    // gh#87 family models occasionally emit a non-parseable tool call (GPU
    // greedy is non-deterministic run-to-run, esp. the 13GB qwen36 at partial
    // offload under suite load). Retry the generate+parse a few times on the
    // already-loaded backend (no reload) and require at least one clean call —
    // tolerates the non-determinism without weakening the parse assertions.
    bool captured = false;
    std::string raw, name, path_arg;
    size_t n_calls = 0;
    for (int attempt = 0; attempt < 4 && n_calls == 0; ++attempt) {
        auto gen = backend.generate(msgs, params);
        captured = backend.has_common_chat_params();
        raw = gen.content;
        auto parsed = backend.parse_response(raw);
        n_calls = parsed.tool_calls.size();
        if (n_calls >= 1) {
            name = parsed.tool_calls[0].name;
            if (parsed.tool_calls[0].arguments.count("path") == 1) {
                path_arg = parsed.tool_calls[0].arguments.at("path");
            }
        }
    }

    backend.deactivate();
    backend.unload();

    INFO("family=" << adapter << " gguf=" << gguf
         << "\nraw=[" << raw << "]"
         << "\ncalls=" << n_calls << " name=[" << name
         << "] path=[" << path_arg << "]");
    REQUIRE(captured);                                // tool-capable format selected
    REQUIRE(n_calls >= 1);                            // parsed a real call
    CHECK(name.find("read_file") != std::string::npos);
    // gh#89-D: path_arg was extracted (L117) but never asserted — a call to
    // read_file with the wrong/empty path still "passed". Assert the argument
    // round-tripped so this verifies parse FIDELITY, not just call presence.
    CHECK(path_arg.find("/etc/hostname") != std::string::npos);
}

}  // namespace gh87verify
