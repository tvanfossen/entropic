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
#include "auto_placement_harness.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path gemma4_a4b_path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') { return {}; }
    return std::filesystem::path(home) / ".entropic" / "models"
         / "gemma-4-26B-A4B-it-UD-IQ2_M.gguf";
}

entropic::ModelConfig make_a4b_config(const std::filesystem::path& gguf) {
    entropic::ModelConfig cfg;
    cfg.path = gguf;
    cfg.adapter = "gemma4";
    cfg.context_length = 4096;
    // v2.14.6: placed by gpu_layers auto (resolve_auto_for_backend_test, in
    // the TEST_CASE): the test machine's VRAM is not knowable in advance, and
    // the old fixed 15 was a split for the retired 13 GB A4B.
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

/**
 * @brief One render → raw decode → parse pass, and what it produced.
 * @version 2.14.6
 */
struct Outcome {
    std::string label;          ///< Variant name, for the report.
    std::string raw;            ///< Raw decoded text.
    std::string content;        ///< Parsed content.
    std::string reasoning;      ///< Parsed reasoning.
    std::vector<entropic::ToolCall> calls; ///< Parsed tool calls.
};

/**
 * @brief Run one variant on the loaded backend.
 * @param backend Active backend with the read_file tool staged.
 * @param label Variant name.
 * @param thinking enable_thinking for the render and decode.
 * @param require REQUIRED tool choice (eager tool-call grammar) vs AUTO.
 * @return What the variant produced.
 * @internal
 * @version 2.14.6
 */
Outcome run_variant(entropic::LlamaCppBackend& backend, const std::string& label,
                    bool thinking, bool require) {
    std::vector<entropic::Message> msgs;
    entropic::Message u;
    u.role = "user";
    u.content = "Use the read_file tool to read /etc/hostname. "
                "Emit only the tool call.";
    msgs.push_back(u);

    entropic::GenerationParams params;
    params.max_tokens = 400;       // room for the closing <tool_call|>
    params.temperature = 0.0f;
    params.enable_thinking = thinking;

    backend.set_require_tool_call(require);
    std::string prompt = backend.render_with_tools(msgs, params);
    // Raw completion: no chat template re-applied, so we decode exactly
    // what render_with_tools produced (and, under REQUIRED, its grammar).
    auto gen = prompt.empty() ? entropic::GenerationResult{}
                              : backend.complete(prompt, params);
    auto parsed = backend.parse_response(gen.content);
    return {label, gen.content, parsed.content, parsed.reasoning_content,
            parsed.tool_calls};
}

}  // namespace

// v2.14.6 (user): a native tool call must come out with thinking on, with it
// off, and under the forced grammar. Was a single thinking-on free-form case;
// in the v2.14.6 gate it once deliberated, then wrote
// `read_file(path="/etc/hostname")` as plain text (0 calls parsed). Only the
// grammar variant is GUARANTEED by the engine; the free-form two measure the
// model's own emission through our render, and a miss there is reported, with
// the raw output, as the failure it is.
TEST_CASE("gh#87 Increment-2: backend render_with_tools → parse_response "
          "extracts a gemma4 tool call (thinking on, off, and grammar-forced)",
          "[gh87][model][e2e]") {
    auto path = gemma4_a4b_path();
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("gemma4 a4b GGUF not present at " + path.string());
    }

    entropic::LlamaCppBackend backend;
    auto cfg = make_a4b_config(path);
    REQUIRE(resolve_auto_for_backend_test(cfg, "gh87_common_chat"));
    REQUIRE(backend.load(cfg));
    REQUIRE(backend.activate());
    backend.set_active_tools(kReadFileToolJson);

    // One load, three variants (the 26B load dominates the cost).
    std::vector<Outcome> outcomes = {
        run_variant(backend, "thinking on, free-form", true, false),
        run_variant(backend, "thinking off, free-form", false, false),
        run_variant(backend, "thinking off, REQUIRED grammar", false, true),
    };
    backend.deactivate();
    backend.unload();

    for (const auto& o : outcomes) {
        INFO("variant: " << o.label << "\nraw model output: " << o.raw
             << "\nparsed tool_calls=" << o.calls.size()
             << " content=[" << o.content << "] reasoning=["
             << o.reasoning << "]");
        // v2.14.6 (user decision): with thinking on, a free-form raw decode may
        // write the call with plain quotes, which neither the pin nor upstream
        // master parses. At this layer the guarantee is that the call is never
        // lost: parsed, or its text kept in content (parse_response logs ERROR).
        // That a thinking-on turn ends in a REAL call is asserted at the
        // engine level, test-v2146-thinking-tool-call.
        if (o.label == "thinking on, free-form" && o.calls.empty()) {
            CHECK(o.content.find("<|tool_call>") != std::string::npos);
            continue;
        }
        // CHECK, not REQUIRE: every variant reports, not just the first miss.
        CHECK(o.calls.size() >= 1);
        if (o.calls.empty()) { continue; }
        CHECK(o.calls[0].name == "read_file");
        CHECK(o.calls[0].arguments.count("path") == 1);
        CHECK(o.calls[0].arguments.at("path").find("/etc/hostname")
              != std::string::npos);
    }
}
