// SPDX-License-Identifier: Apache-2.0
/**
 * @file gemma4_adapter.cpp
 * @brief Gemma 4 adapter implementation (v2.1.9, gh#46).
 *
 * Permissive multi-format tool parser, GGUF-embedded chat template,
 * shared base-class think-block stripping. The v2.3.8 work (gh#69)
 * resolved the open question on Gemma 4's native tool-call syntax: the
 * model emits a ChatML-style channel with the asymmetric open header
 * `<|im_start|>tool_call` and a plain `</tool_call>` close. The v2.3.11
 * work (gh#72) cuts the emit at the first fabricated turn boundary so
 * the parser cannot dispatch tool calls from the model's self-driven
 * multi-turn continuations.
 *
 * @version 2.3.11
 */

#include "gemma4_adapter.h"

#include <nlohmann/json.hpp>

#include <regex>

namespace entropic {

/**
 * @brief Cut model output at the first fabricated turn boundary.
 *
 * Gemma 4 (especially E4B Q8 / Q4_K_M, gh#72) frequently emits a
 * complete synthetic multi-turn exchange inside ONE assistant turn:
 *
 *   <real reply><|im_end|><|im_start|>user
 *   <fabricated user content><|im_end|><|im_start|>assistant
 *   <fabricated assistant reply with another tool_call>
 *
 * Without truncation, `parse_tool_calls` extracts the fabricated tool
 * call and the engine dispatches a request the real parent never sent.
 * The cut runs BEFORE tool extraction so dispatch is prevented even
 * when the consumer-side post-clean catches the visible-text leak.
 *
 * Rule: cut at the first `<|im_end|>` (or `<end_of_turn>`) that is
 * followed by a NON-tool_call channel opener (`<|im_start|>user`,
 * `<|im_start|>assistant`, `<|im_start|>system`, or `<start_of_turn>`).
 * Legitimate multi-tool-call emits separate channels with
 * `<|im_start|>tool_call`, which we keep — see the gh#69 acceptance
 * scenarios.
 *
 * @param content Raw model output.
 * @return Content truncated at the fabrication boundary, or the input
 *         unchanged when no boundary is found.
 * @utility
 * @version 2.3.11
 */
static std::string cut_at_fabricated_turn(const std::string& content) {
    static const std::regex kFabricatedTurn(
        R"((?:<\|im_end\|?>|<end_of_turn>)\s*)"
        R"((?:<\|im_start\|?>(?:user|assistant|system)|<start_of_turn>(?:user|model)))");
    std::smatch m;
    if (std::regex_search(content, m, kFabricatedTurn)) {
        return content.substr(0, static_cast<size_t>(m.position()));
    }
    return content;
}

/**
 * @brief Normalize Gemma 4 E4B Q8 JSON5-ish args into strict JSON.
 *
 * Three transforms (gh#73):
 *   1. `<|"|>` (the model's confused escape sequence for `"`) → `"`.
 *   2. Bare keys after `{` or `,` get quoted: `{key:` → `{"key":`.
 *   3. Trailing commas before `}` or `]` get dropped.
 *
 * Pure regex pass — non-throwing. Returns the transformed string;
 * caller is responsible for catching `nlohmann::json::parse` failures
 * when the input still isn't valid JSON after normalization.
 * @utility
 * @version 2.3.12
 */
static std::string normalize_call_args_json(const std::string& args_body) {
    std::string s = args_body;
    // 1. <|"|> → "
    static const std::regex kQuoteEscape(R"(<\|"\|>)");
    s = std::regex_replace(s, kQuoteEscape, "\"");
    // 2. Bare keys: insert quotes around identifier when preceded by
    //    `{`, `,`, or whitespace following one of those, and followed
    //    by `:`. Keys are [A-Za-z_][\w]*.
    static const std::regex kBareKey(R"(([{,]\s*)([A-Za-z_][\w]*)\s*:)");
    s = std::regex_replace(s, kBareKey, "$1\"$2\":");
    // 3. Drop trailing commas before close brace/bracket.
    static const std::regex kTrailingComma(R"(,\s*([}\]]))");
    s = std::regex_replace(s, kTrailingComma, "$1");
    return s;
}

/**
 * @brief Try to materialize a ToolCall from a `call:` body's args.
 *
 * Normalizes the args body, parses it as JSON, and populates the
 * ToolCall's arguments map + canonical arguments_json dump. Returns
 * `false` when normalization yields un-parseable JSON or a non-object
 * root — caller drops the match in that case.
 *
 * @param name Tool name already captured from the wrapper.
 * @param args_body Raw `{...}` body content (without the braces).
 * @param out Populated on success.
 * @return true when args parsed successfully into an object.
 * @utility
 * @version 2.3.12
 */
static bool build_call_prefix_tool(
    const std::string& name,
    const std::string& args_body,
    ToolCall& out)
{
    // Wrap in `{}` BEFORE normalize so the bare-key regex (which
    // requires `{` or `,` before each key) catches the leading key
    // too — without the wrapper the first key has no preceding `{`.
    std::string normalized =
        normalize_call_args_json("{" + args_body + "}");
    try {
        auto j = nlohmann::json::parse(normalized);
        if (!j.is_object()) { return false; }
        out.name = name;
        out.arguments_json = j.dump();
        for (auto kv = j.begin(); kv != j.end(); ++kv) {
            out.arguments[kv.key()] = kv.value().is_string()
                ? kv.value().get<std::string>()
                : kv.value().dump();
        }
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

/**
 * @brief Parse Gemma 4 E4B Q8 `<|tool_call>call:NAME{args}<tool_call|>` form.
 *
 * Third fallback parser (after tagged + bare-JSON) for the malformed
 * tool-call wrapper observed in production on Gemma 4 E4B Q8 (gh#73).
 * Three deviations from gh#69's accepted set:
 *   - `call:` prefix before the tool name.
 *   - Non-JSON args (`{student_id:2}`, no quotes around keys).
 *   - Asymmetric close `<tool_call|>` (pipe-before-`>`).
 *
 * Each parsed call gets its full canonical JSON written to
 * `ToolCall::arguments_json` so dispatch-passthrough consumers see a
 * normalized shape. Argument keys are exposed in the `arguments` map
 * as string-stringified JSON values.
 *
 * @param content Raw model output (after `cut_at_fabricated_turn`).
 * @return Vector of recovered tool calls. Empty when no matching
 *         wrapper is found OR every match's args fail to normalize.
 * @utility
 * @version 2.3.12
 */
static std::vector<ToolCall> parse_call_prefix_tool_calls(
    const std::string& content)
{
    static const std::regex kCallPrefix(
        R"(<\|tool_call\|?>\s*call\s*:\s*([\w.]+)\s*\{([\s\S]*?)\}\s*<tool_call\|>)");
    std::vector<ToolCall> calls;
    auto begin = std::sregex_iterator(
        content.begin(), content.end(), kCallPrefix);
    auto end = std::sregex_iterator{};
    for (auto it = begin; it != end; ++it) {
        ToolCall tc;
        if (build_call_prefix_tool((*it)[1].str(), (*it)[2].str(), tc)) {
            calls.push_back(std::move(tc));
        }
    }
    return calls;
}

/**
 * @brief Parse tool calls from Gemma 4 output.
 *
 * Tries tagged JSON first (`<tool_call>{...}</tool_call>` and the
 * asymmetric `<|tool_call>` / `<|im_start|>tool_call` channel variants
 * the base parser now accepts), then bare-JSON lines as a fallback.
 * The base class handles malformed JSON recovery transparently when
 * either path is exercised. The v2.3.11 fabricated-turn cut runs first
 * so neither path can act on post-turn fabrication.
 *
 * @param content Raw model output.
 * @return ParseResult with cleaned content and any extracted calls.
 * @internal
 * @version 2.3.11
 */
ParseResult Gemma4Adapter::parse_tool_calls(const std::string& content) const {
    // gh#72 (v2.3.11): cut fabricated multi-turn continuations BEFORE
    // tool-call extraction so the engine never dispatches calls the
    // real parent never authorized.
    std::string trimmed = cut_at_fabricated_turn(content);

    ParseResult result;

    auto calls = parse_tagged_tool_calls(trimmed);
    if (calls.empty()) {
        calls = parse_bare_json_tool_calls(trimmed);
    }
    // gh#73 (v2.3.12): third fallback for the E4B Q8 malformed
    // `<|tool_call>call:NAME{args}<tool_call|>` wrapper. Runs only
    // when the tagged + bare-JSON paths both came up empty so we
    // don't double-extract calls that the canonical paths handled.
    if (calls.empty()) {
        calls = parse_call_prefix_tool_calls(trimmed);
    }
    result.tool_calls = std::move(calls);

    // gh#65 (v2.3.3) / gh#69 (v2.3.8) / gh#73 (v2.3.12): match the
    // asymmetric open + close variants here too so the cleaned_content
    // (what the user sees) doesn't leave stray
    // `<|tool_call>{json}</tool_call>`, `<|im_start|>tool_call ... </tool_call>`,
    // or `<|tool_call>call:NAME{...}<tool_call|>` markup behind when
    // Gemma 4 emits the pipe-prefixed, channel-header, or call-prefix
    // form. Mirror the openings + closings accepted by the parsers.
    std::string cleaned = std::regex_replace(trimmed,
        std::regex(R"((?:<tool_call>|<\|tool_call\|?>|<\|im_start\|>tool_call))"
                   R"(\s*[\s\S]*?\s*(?:</tool_call>|<tool_call\|>))"),
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
