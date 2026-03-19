/**
 * @file generic_adapter.h
 * @brief GenericAdapter — default ChatML adapter with JSON tool calls.
 *
 * Used as fallback for unknown adapter names.
 *
 * @version 1.8.2
 */

#pragma once

#include <entropic/inference/adapters/adapter_base.h>

namespace entropic {

/**
 * @brief Generic adapter using <tool_call>JSON</tool_call> format.
 * @version 1.8.2
 */
class GenericAdapter : public ChatAdapter {
public:
    using ChatAdapter::ChatAdapter;

    /**
     * @brief Chat format: ChatML.
     * @version 1.8.2
     */
    std::string chat_format() const override { return "chatml"; }

    /**
     * @brief Parse tagged JSON tool calls.
     * @param content Raw model output.
     * @return ParseResult.
     * @version 1.8.2
     */
    ParseResult parse_tool_calls(const std::string& content) const override;
};

} // namespace entropic
