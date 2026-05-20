// SPDX-License-Identifier: Apache-2.0
/**
 * @file gemma4_adapter.cpp
 * @brief Gemma 4 adapter implementation (v2.1.9, gh#46).
 *
 * Permissive multi-format tool parser, GGUF-embedded chat template,
 * shared base-class think-block stripping. See header for the open
 * question on Gemma 4's native tool-call syntax — to be refined at
 * the v2.1.9 model-test phase.
 *
 * @version 2.1.9
 */

#include "gemma4_adapter.h"

#include <regex>

namespace entropic {

/**
 * @brief Parse tool calls from Gemma 4 output.
 *
 * Tries tagged JSON first (`<tool_call>{...}</tool_call>`), then
 * bare-JSON lines as a fallback. The base class handles malformed
 * JSON recovery transparently when either path is exercised.
 *
 * @param content Raw model output.
 * @return ParseResult with cleaned content and any extracted calls.
 * @internal
 * @version 2.1.9
 */
ParseResult Gemma4Adapter::parse_tool_calls(const std::string& content) const {
    ParseResult result;

    auto calls = parse_tagged_tool_calls(content);
    if (calls.empty()) {
        calls = parse_bare_json_tool_calls(content);
    }
    result.tool_calls = std::move(calls);

    // gh#65 (v2.3.3): match the asymmetric open variants here too so
    // the cleaned_content (what the user sees) doesn't leave stray
    // `<|tool_call>{json}</tool_call>` markup behind when Gemma 4
    // emits the pipe-prefixed form. Mirror the openings accepted by
    // parse_tagged_tool_calls.
    std::string cleaned = std::regex_replace(content,
        std::regex(R"((?:<tool_call>|<\|tool_call\|?>)\s*[\s\S]*?\s*</tool_call>)"),
        "");
    cleaned = strip_think_blocks(cleaned);
    result.cleaned_content = std::move(cleaned);
    return result;
}

} // namespace entropic
