// SPDX-License-Identifier: Apache-2.0
/**
 * @file gemma4_adapter.h
 * @brief Gemma4Adapter — the fallback parse path for Gemma-4 tiers (gh#108).
 *
 * @par Why this exists
 * Gemma-4 tool calls are parsed by llama.cpp's dedicated `PEG_GEMMA4`
 * grammar, which is why `adapter_registry` deliberately had no gemma4 entry.
 * But that path is gated on `common_chat_parse_reliable()`, which requires a
 * TOOLED render to have captured a parser arena. A toolless generate —
 * a bare `entropic.ask`, a validator critique, any consumer not staging
 * tools — has no arena, falls through to the adapter branch, and before
 * v2.10.3 landed on `GenericAdapter`. Generic strips `<think>`, a marker
 * Gemma-4 never emits, and left `<|channel>…<channel|>` fully intact, so raw
 * reasoning reached consumers and conversation history on every path: plain,
 * streaming, MTP, batch.
 *
 * So this adapter is a *fallback*, not a replacement for the template path.
 * Tool-call extraction still defers to the base tagged/bare-JSON primitives —
 * `PEG_GEMMA4` remains primary whenever an arena exists. What this class
 * exists to own is the family's reasoning delimiters, declared once via
 * thinking_markers() and consumed by both the buffered strip and the live
 * StreamThinkFilter.
 *
 * @version 2.10.3
 */

#pragma once

#include <entropic/inference/adapters/adapter_base.h>

namespace entropic {

/**
 * @brief Fallback adapter for Gemma-4 tiers; owns the `<|channel>` markers.
 * @version 2.10.3
 */
class Gemma4Adapter : public ChatAdapter {
public:
    using ChatAdapter::ChatAdapter;

    /**
     * @brief Chat format identifier.
     * @return The string "gemma4".
     * @utility
     * @version 2.10.3
     */
    std::string chat_format() const override { return "gemma4"; }

    /**
     * @brief Gemma-4 QAT reasoning delimiters.
     *
     * Note the close marker is `<channel|>`, not a mirrored `</|channel>` —
     * matching the live emission and `strip_thinking_channels`, whose
     * behaviour this consolidates.
     *
     * @return `{"<|channel>", "<channel|>"}`.
     * @utility
     * @version 2.10.3
     */
    ThinkMarkers thinking_markers() const override {
        return {"<|channel>", "<channel|>"};
    }

    /**
     * @brief Fallback parse for when no common_chat arena is available.
     * @param content Raw model output.
     * @return ParseResult with reasoning stripped and any tool calls found.
     * @version 2.10.3
     */
    ParseResult parse_tool_calls(const std::string& content) const override;
};

} // namespace entropic
