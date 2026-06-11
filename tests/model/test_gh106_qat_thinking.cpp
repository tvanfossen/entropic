// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh106_qat_thinking.cpp
 * @brief gh#106 (v2.9.0): the Gemma 4 QAT models are THINKING models — they emit
 *        a `<|channel>thought ... <channel|>` reasoning channel before the final
 *        answer/tool-call. This OBSERVES the exact tool-call output structure on
 *        a QAT GGUF (generous token budget so thinking + the call both fit), to
 *        decide the fix: strip the channel (if the call is PEG_GEMMA4 after
 *        `<channel|>`) vs. parse-inside-thinking (if the call is JSON inside it).
 *        Will become the RED/GREEN gate for strip_thinking_channels.
 * @version 2.9.0
 */

#include "gh87_verify_helpers.h"

#include <entropic/types/tool_call.h>

#include <cstdio>

TEST_CASE("gh#106 observe: QAT thinking-model tool-call structure",
          "[model][gh106][qat][thinking]") {
    auto path = gh87verify::model_path("gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("standard-QAT-Q4 GGUF not present: " + path.string());
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

    backend.set_active_tools(gh87verify::read_file_tool_json());

    std::vector<entropic::Message> msgs;
    {
        entropic::Message u;
        u.role = "user";
        u.content = "You must call the read_file tool to read /etc/hostname. "
                    "Respond with the tool call.";
        msgs.push_back(u);
    }
    entropic::GenerationParams params;
    params.max_tokens = 600;        // generous: thinking + the call must both fit
    params.temperature = 0.0f;
    params.enable_thinking = true;  // these models think when tools are present

    auto gen = backend.generate(msgs, params);
    bool captured = backend.has_common_chat_params();
    auto parsed = backend.parse_response(gen.content);

    backend.deactivate();
    backend.unload();

    std::printf("\n===gh106 QAT RAW (%zu chars)===\n%s\n===PARSE: %zu tool_calls, "
                "cleaned=[%s]===\n",
                gen.content.size(), gen.content.c_str(),
                parsed.tool_calls.size(),
                parsed.content.c_str());
    INFO("captured(has_common_chat_params)=" << captured
         << " calls=" << parsed.tool_calls.size());
    REQUIRE(captured);
    REQUIRE(gen.error_code == 0);
    // Tool-call DOES extract once the budget lets the model finish thinking —
    // common_chat finds <|tool_call> after the <channel|> close (PEG_GEMMA4).
    REQUIRE(parsed.tool_calls.size() >= 1);
    CHECK(parsed.tool_calls[0].name.find("read_file") != std::string::npos);
    // gh#106 FIX TARGET: the thinking channel must NOT leak into content.
    // RED before strip_thinking_channels (content == "<|channel>thought…"),
    // GREEN after.
    CHECK(parsed.content.find("<|channel>") == std::string::npos);
    CHECK(parsed.content.find("<channel|>") == std::string::npos);
}
