// SPDX-License-Identifier: Apache-2.0
/**
 * @file qwen36_adapter.h
 * @brief Qwen 3.6 chat adapter (v2.1.9, gh#45).
 *
 * @par Tool-call format
 * Qwen 3.6 emits the same XML function-call format the vLLM
 * `qwen3_coder` parser recognises:
 * @code
 * <tool_call>
 * <function=filesystem.read_file>
 * <parameter=path>/src/main.cpp</parameter>
 * </function>
 * </tool_call>
 * @endcode
 * That format overlaps with Qwen 3.5 today. The class remains
 * distinct per the v2.1.9 "no version lumping" decision — template
 * particulars across the 3.x line can diverge later; a
 * `QwenBaseAdapter` extraction is a refactor for when overlap is
 * proved, not a precondition.
 *
 * @par Reasoning
 * Qwen 3.6 is a thinking model; reasoning is wrapped in
 * `<think>...</think>` blocks. Stripping and extraction reuse the
 * base class primitives.
 *
 * @par Vision
 * Qwen 3.6 ships paired with an `mmproj-F16.gguf` (see registry
 * key `qwen3_6_a3b_mmproj`) and follows the OpenAI content-array
 * convention for image parts. The vision system-prompt extension
 * mirrors Qwen 3.5.
 *
 * Internal to inference .so.
 *
 * @version 2.1.9
 */

#pragma once

#include <entropic/inference/adapters/adapter_base.h>

#include <unordered_map>

namespace entropic {

/**
 * @brief Qwen 3.6 MoE chat adapter.
 *
 * Overrides: XML tool call parsing (qwen3_coder format), `<tools>`
 * tag formatting, `<tool_response>` result wrapping, vision system
 * prompt extension. Reuses base-class think-block handling.
 *
 * @version 2.1.9
 */
class Qwen36Adapter : public ChatAdapter {
public:
    using ChatAdapter::ChatAdapter;

    /**
     * @brief Chat format: ChatML.
     * @return The string "chatml".
     * @utility
     * @version 2.1.9
     */
    std::string chat_format() const override { return "chatml"; }

    /**
     * @brief Parse XML function calls; fall back to tagged JSON.
     * @param content Raw model output.
     * @return ParseResult with cleaned content and tool calls.
     * @version 2.1.9
     */
    ParseResult parse_tool_calls(const std::string& content) const override;

    /**
     * @brief Wrap tool result in `<tool_response>` tags.
     * @param tool_call Executed tool call.
     * @param result Execution result text.
     * @return User-role message containing the wrapped result.
     * @version 2.1.9
     */
    Message format_tool_result(
        const ToolCall& tool_call,
        const std::string& result) const override;

protected:
    /**
     * @brief Format tools as `<tools>...</tools>` with OpenAI function JSON.
     * @param tool_jsons Tool definition JSON strings.
     * @return Formatted tools section to inject into the system prompt.
     * @version 2.1.9
     */
    std::string format_tools(
        const std::vector<std::string>& tool_jsons) const override;

private:
    /**
     * @brief Parse `<function=name><parameter=key>value</parameter></function>` blocks.
     * @param content Model output.
     * @return Vector of parsed tool calls.
     * @internal
     * @version 2.1.9
     */
    std::vector<ToolCall> parse_xml_function_calls(
        const std::string& content) const;

    /**
     * @brief Extract `<parameter=...>...</parameter>` pairs from a function body.
     * @param func_body Function body text.
     * @return Map of parameter key -> value.
     * @internal
     * @version 2.1.9
     */
    std::unordered_map<std::string, std::string> extract_xml_parameters(
        const std::string& func_body) const;

    /**
     * @brief Strip tool calls and think blocks from content.
     * @param content Raw model output.
     * @return Cleaned content.
     * @internal
     * @version 2.1.9
     */
    std::string clean_content(const std::string& content) const;

public:
    /* ── Vision / multimodal ──────────────────────────────── */

    /**
     * @brief Append vision instructions to the system prompt when active.
     * @param base_system Base system prompt text.
     * @param has_vision Whether vision capability is available for the tier.
     * @return Formatted system prompt.
     * @version 2.1.9
     */
    std::string format_system_with_vision(
        const std::string& base_system,
        bool has_vision) const override;

    /**
     * @brief Format multimodal content parts (OpenAI-native).
     *
     * Qwen 3.6 uses the same OpenAI content-array convention as
     * Qwen 3.5 — early fusion, no special image tags.
     *
     * @param parts Content parts from a message.
     * @return JSON string in the OpenAI content-array format.
     * @version 2.1.9
     */
    std::string format_content_parts(
        const std::vector<ContentPart>& parts) const override;
};

} // namespace entropic
