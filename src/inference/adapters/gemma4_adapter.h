// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file gemma4_adapter.h
 * @brief Gemma 4 chat adapter (v2.1.9, gh#46).
 *
 * Covers the Gemma 4 instruct family: 26B-A4B, E4B, E2B variants.
 * All three share the same chat template structure and channel-based
 * thinking convention, so a single adapter class handles them.
 *
 * @par Chat template
 * The GGUF-embedded template uses Gemma's `<|channel>thought\n...
 * <channel|>` markers for the reasoning/answer split. We do not
 * replicate the template here — `chat_format()` returns the empty
 * string so llama.cpp applies the GGUF-stored template directly.
 *
 * @par Tool-call format (open question — see Implementation Log)
 * Upstream documentation surveyed at v2.1.9 session start describes
 * Gemma 4 as having "native function calling" but does not show the
 * exact raw output syntax. Until the format is verified empirically
 * via the v2.1.9 model tests, this adapter uses a permissive parser:
 *   1. Primary: tagged JSON `<tool_call>{"name":..., "arguments":...}
 *      </tool_call>` — base class `parse_tagged_tool_calls`. We bias
 *      the model toward this format via the tool-injection prompt.
 *   2. Fallback: bare-JSON lines containing `"name"` — base class
 *      `parse_bare_json_tool_calls`.
 *   3. Final fallback: malformed-JSON recovery via the base class.
 *
 * When the model-test phase nails down the actual format, override
 * `parse_tool_calls` here with a Gemma-specific extractor and bump
 * `@version`.
 *
 * @par Reasoning
 * Gemma 4 emits a channel-tagged thought block. If the GGUF template
 * surfaces those tokens as `<think>...</think>` (common llama.cpp
 * convention for thinking models), the base-class `strip_think_blocks`
 * cleans them. If a different marker shape leaks through, that is
 * addressed in the same model-test loop.
 *
 * Internal to inference .so.
 *
 * @version 2.1.9
 */

#pragma once

#include <entropic/inference/adapters/adapter_base.h>

namespace entropic {

/**
 * @brief Gemma 4 chat adapter (covers A4B / E4B / E2B variants).
 *
 * Uses the GGUF-embedded chat template via `chat_format()=""` and
 * a permissive multi-format tool-call parser pending empirical
 * verification.
 *
 * @version 2.1.9
 */
class Gemma4Adapter : public ChatAdapter {
public:
    using ChatAdapter::ChatAdapter;

    /**
     * @brief Chat format: GGUF-embedded template.
     * @return Empty string — llama.cpp applies the stored template.
     * @utility
     * @version 2.1.9
     */
    std::string chat_format() const override { return ""; }

    /**
     * @brief Parse tool calls via tagged JSON, falling back to bare JSON.
     *
     * Permissive strategy pending empirical confirmation of Gemma 4's
     * native tool-call syntax. Strips `<think>` blocks from content.
     *
     * @param content Raw model output.
     * @return ParseResult.
     * @version 2.1.9
     */
    ParseResult parse_tool_calls(const std::string& content) const override;
};

} // namespace entropic
