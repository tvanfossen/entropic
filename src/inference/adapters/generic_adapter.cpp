// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file generic_adapter.cpp
 * @brief GenericAdapter implementation.
 * @version 1.8.2
 */

#include "generic_adapter.h"

#include <regex>

namespace entropic {

/**
 * @brief Parse tagged JSON tool calls.
 *
 * Uses base class parse_tagged_tool_calls for
 * <tool_call>JSON</tool_call> format. Strips tags from content.
 *
 * @param content Raw model output.
 * @return ParseResult.
 * @internal
 * @version 1.8.2
 */
ParseResult GenericAdapter::parse_tool_calls(const std::string& content) const {
    ParseResult result;
    result.tool_calls = parse_tagged_tool_calls(content);

    // Remove <tool_call>...</tool_call> blocks from content
    std::regex pattern(R"(<tool_call>\s*[\s\S]*?\s*</tool_call>)");
    result.cleaned_content = std::regex_replace(content, pattern, "");

    // Strip think blocks
    result.cleaned_content = strip_think_blocks(result.cleaned_content);
    return result;
}

} // namespace entropic
