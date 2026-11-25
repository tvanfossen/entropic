// SPDX-License-Identifier: Apache-2.0
/**
 * @file gemma4_dropped_tool_text.h
 * @brief Keep a Gemma 4 tool-call span the parser could not read (v2.14.6).
 *
 * v2.14.6 gate (gh87-backend-common-chat, thinking on): the model wrote
 * `<|tool_call>call:read_file{path: "/etc/hostname"}<tool_call|>`, with plain
 * quotes where Gemma 4 uses `<|"|>`. At the pinned llama.cpp, common_chat_parse
 * returned no call and kept only the thought block as content, and
 * strip_thinking_channels then moved that into reasoning. The call vanished
 * with no log line. Upstream master still requires `<|"|>` around string
 * values, so there is no parse fix to mirror. The invariant is ours: call text
 * the parser did not read stays visible, where the engine treats it as prose
 * and nudges, and where an operator can see it.
 *
 * @version 2.14.6
 */

#pragma once

#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Call spans in @p raw that the parse neither read nor kept.
 *
 * A span runs from `<|tool_call>` through `<tool_call|>`, or to the end of the
 * text when the closing marker never arrived (max_tokens). A span already
 * present in @p content was kept and is not reported. Callers run this only
 * when the parse produced no tool calls.
 *
 * @param raw Raw model emission.
 * @param content Content the parse kept.
 * @return Dropped spans, verbatim, in order; empty when nothing was lost.
 * @utility
 * @version 2.14.6
 */
inline std::vector<std::string> gemma4_dropped_tool_spans(
    const std::string& raw, const std::string& content) {
    static const std::string kOpen = "<|tool_call>";
    static const std::string kClose = "<tool_call|>";
    std::vector<std::string> spans;
    std::size_t pos = raw.find(kOpen);
    while (pos != std::string::npos) {
        const std::size_t close = raw.find(kClose, pos + kOpen.size());
        const std::size_t end = (close == std::string::npos)
            ? raw.size() : close + kClose.size();
        std::string span = raw.substr(pos, end - pos);
        if (content.find(span) == std::string::npos) {
            spans.push_back(std::move(span));
        }
        pos = raw.find(kOpen, end);
    }
    return spans;
}

/**
 * @brief Append dropped spans to content, one per line.
 * @param content [in,out] Parsed content.
 * @param spans Spans from gemma4_dropped_tool_spans.
 * @utility
 * @version 2.14.6
 */
inline void restore_dropped_tool_spans(std::string& content,
                                       const std::vector<std::string>& spans) {
    for (const auto& span : spans) {
        if (!content.empty() && content.back() != '\n') { content += '\n'; }
        content += span;
    }
}

} // namespace entropic
