// SPDX-License-Identifier: Apache-2.0
/**
 * @file gemma4_adapter.cpp
 * @brief Gemma4Adapter implementation (gh#108).
 * @version 2.10.3
 */

#include "gemma4_adapter.h"

namespace entropic {

/**
 * @brief Fallback parse for when no common_chat arena is available.
 *
 * Reasoning removal comes from the base marker-driven strip, which reads
 * thinking_markers() — the whole reason this class exists.
 *
 * For tool calls this reuses the base bare-JSON recovery rather than adding a
 * hand-rolled Gemma grammar: `PEG_GEMMA4` stays primary whenever a tooled
 * render captured an arena, and a toolless call has no staged tools to invoke
 * in the first place, so the realistic yield here is zero calls plus clean
 * content. `recover_action_envelope_calls` still covers the gh#88 case where
 * a primed model parrots a `{"action":...}` envelope.
 *
 * @param content Raw model output.
 * @return ParseResult.
 * @internal
 * @version 2.10.3
 */
ParseResult Gemma4Adapter::parse_tool_calls(const std::string& content) const {
    ParseResult result;
    result.tool_calls = recover_action_envelope_calls(content);
    result.cleaned_content = strip_think_blocks(content);
    return result;
}

} // namespace entropic
