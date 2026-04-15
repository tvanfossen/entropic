// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file qwen35_adapter.h
 * @brief Qwen 3.5 chat adapter.
 *
 * @par XML tool call format
 * @code
 * <tool_call>
 * <function=filesystem.read_file>
 * <parameter=path>/src/main.cpp</parameter>
 * </function>
 * </tool_call>
 * @endcode
 *
 * @par Tool definitions use OpenAI function format in <tools> tags.
 * @par Tool results use <tool_response>...</tool_response> wrapping.
 *
 * Internal to inference .so.
 *
 * @version 1.8.2
 */

#pragma once

#include <entropic/inference/adapters/adapter_base.h>

#include <unordered_map>

namespace entropic {

/**
 * @brief Qwen 3.5 MoE chat adapter (20% override layer).
 *
 * Overrides: XML tool call parsing, <tools> tag formatting,
 * <tool_response> result wrapping.
 *
 * @version 1.8.2
 */
class Qwen35Adapter : public ChatAdapter {
public:
    using ChatAdapter::ChatAdapter;

    /**
     * @brief Chat format: ChatML.
     * @return The string "chatml".
     * @utility
     * @version 1.8.2
     */
    std::string chat_format() const override { return "chatml"; }

    /**
     * @brief Parse XML function calls, fallback to tagged JSON.
     * @param content Raw model output.
     * @return ParseResult with cleaned content and tool calls.
     * @version 1.8.2
     */
    ParseResult parse_tool_calls(const std::string& content) const override;

    /**
     * @brief Wrap tool result in <tool_response> tags.
     * @version 1.8.2
     */
    Message format_tool_result(
        const ToolCall& tool_call,
        const std::string& result) const override;

protected:
    /**
     * @brief Format tools in <tools> tags with OpenAI function JSON.
     * @version 1.8.2
     */
    std::string format_tools(
        const std::vector<std::string>& tool_jsons) const override;

private:
    /**
     * @brief Parse <function=name><parameter=key>value</parameter></function>.
     * @version 1.8.2
     */
    std::vector<ToolCall> parse_xml_function_calls(
        const std::string& content) const;

    /**
     * @brief Extract parameter pairs from function body.
     * @version 1.8.2
     */
    std::unordered_map<std::string, std::string> extract_xml_parameters(
        const std::string& func_body) const;

    /**
     * @brief Remove tool calls and think blocks from content.
     * @version 1.8.2
     */
    std::string clean_content(const std::string& content) const;

public:
    /* ── Vision / multimodal (v1.9.11) ────────────────────── */

    /**
     * @brief Qwen3.5 vision system prompt extension.
     *
     * When has_vision is true, appends vision capability instructions.
     * Otherwise returns base_system unchanged.
     *
     * @param base_system Base system prompt text.
     * @param has_vision Whether the model has vision capability.
     * @return Formatted system prompt.
     * @version 1.9.11
     */
    std::string format_system_with_vision(
        const std::string& base_system,
        bool has_vision) const override;

    /**
     * @brief Qwen3.5 content part formatting.
     *
     * Qwen3.5 uses the OpenAI content array format natively
     * (early fusion, no special image tags). Default implementation
     * is sufficient — override exists for documentation and future tweaks.
     *
     * @param parts Content parts from a message.
     * @return JSON string in OpenAI content array format.
     * @version 1.9.11
     */
    std::string format_content_parts(
        const std::vector<ContentPart>& parts) const override;
};

} // namespace entropic
