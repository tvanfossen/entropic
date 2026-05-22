// SPDX-License-Identifier: Apache-2.0
/**
 * @file gemma4_adapter.cpp
 * @brief Gemma 4 adapter implementation (v2.1.9, gh#46).
 *
 * Permissive multi-format tool parser, GGUF-embedded chat template,
 * shared base-class think-block stripping. The v2.3.8 work (gh#69)
 * resolved the open question on Gemma 4's native tool-call syntax: the
 * model emits a ChatML-style channel with the asymmetric open header
 * `<|im_start|>tool_call` and a plain `</tool_call>` close.
 *
 * @version 2.3.8
 */

#include "gemma4_adapter.h"

#include <regex>

namespace entropic {

/**
 * @brief Parse tool calls from Gemma 4 output.
 *
 * Tries tagged JSON first (`<tool_call>{...}</tool_call>` and the
 * asymmetric `<|tool_call>` / `<|im_start|>tool_call` channel variants
 * the base parser now accepts), then bare-JSON lines as a fallback.
 * The base class handles malformed JSON recovery transparently when
 * either path is exercised.
 *
 * @param content Raw model output.
 * @return ParseResult with cleaned content and any extracted calls.
 * @internal
 * @version 2.3.8
 */
ParseResult Gemma4Adapter::parse_tool_calls(const std::string& content) const {
    ParseResult result;

    auto calls = parse_tagged_tool_calls(content);
    if (calls.empty()) {
        calls = parse_bare_json_tool_calls(content);
    }
    result.tool_calls = std::move(calls);

    // gh#65 (v2.3.3) / gh#69 (v2.3.8): match the asymmetric open
    // variants here too so the cleaned_content (what the user sees)
    // doesn't leave stray `<|tool_call>{json}</tool_call>` or
    // `<|im_start|>tool_call ... </tool_call>` markup behind when
    // Gemma 4 emits the pipe-prefixed or channel-header form. Mirror
    // the openings accepted by parse_tagged_tool_calls.
    std::string cleaned = std::regex_replace(content,
        std::regex(R"((?:<tool_call>|<\|tool_call\|?>|<\|im_start\|>tool_call))"
                   R"(\s*[\s\S]*?\s*</tool_call>)"),
        "");
    cleaned = strip_think_blocks(cleaned);

    // gh#68 (v2.3.5): scrub Gemma 4 chat-template turn-boundary
    // markers from surface content. The v2.3.4 detokenize special=false
    // change does NOT catch these — Gemma 4 emits them as multi-token
    // *regular* surface tokens (e.g. `<`, `|`, `im`, `_end`, `|>`)
    // that llama.cpp doesn't classify as special, so they slip through
    // detokenize unchanged. Same family of bug as gh#65's asymmetric
    // `<|tool_call>`: chat-template artifacts spelled by regular tokens.
    //
    // Without this scrub, `<|im_end|>` leaked into the assistant
    // content stream, echoed back into the next turn's prompt, and
    // the engine's "no tool call this iteration" retry cascade fired
    // until iteration cap.
    //
    // Asymmetric variants are matched explicitly (`\|?`) for parity
    // with the gh#65 tool-call regex — Gemma 4's tokenizer surface
    // drops the trailing `|>` on some emit paths.
    //
    // gh#69 (v2.3.8): `tool_call` joins the channel-role list so a
    // stray `<|im_start|>tool_call` header (one whose `</tool_call>`
    // close didn't pair up, e.g. a truncated emit) is scrubbed instead
    // of leaking into the assistant-visible body. The paired
    // `<|im_start|>tool_call ... </tool_call>` block is already removed
    // above; this catches the degenerate unpaired header.
    static const std::regex kGemmaTemplateMarkers(
        R"(<\|im_end\|?>|<\|im_start\|?>(?:user|assistant|system|tool_call)?|)"
        R"(<end_of_turn>|<start_of_turn>(?:user|model)?)");
    cleaned = std::regex_replace(cleaned, kGemmaTemplateMarkers, "");

    result.cleaned_content = std::move(cleaned);
    return result;
}

} // namespace entropic
