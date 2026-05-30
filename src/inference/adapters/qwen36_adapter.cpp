// SPDX-License-Identifier: Apache-2.0
/**
 * @file qwen36_adapter.cpp
 * @brief Qwen 3.6 adapter implementation (v2.1.9, gh#45).
 *
 * Mirrors `Qwen35Adapter` structurally — Qwen 3.6 uses the same
 * qwen3_coder XML tool-call format and the same vision content-array
 * convention. The class is intentionally distinct so future template
 * divergence across the Qwen 3.x line can be absorbed without churning
 * Qwen35Adapter callers.
 *
 * @version 2.1.9
 */

#include "qwen36_adapter.h"

#include "xml_parameter_parser.h"

#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <regex>
#include <sstream>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.adapter.qwen36");

/// @brief Shared continuation text appended after tool results.
constexpr const char* TOOL_RESULT_SUFFIX =
    "Continue. Batch multiple tool calls in one response when possible.";

/**
 * @brief Generate a simple counter-based tool-call id.
 * @return ID string of the form "tc-N".
 * @internal
 * @version 2.1.9
 */
std::string generate_uuid() {
    static std::atomic<uint64_t> counter{0};
    return "tc-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

/// @brief Vision instruction appended to the system prompt when active.
constexpr const char* VISION_INSTRUCTION =
    "\n\nYou can see and analyze images. When the user shares an "
    "image, describe what you observe before responding to their "
    "question.";

} // anonymous namespace

// ── Tool call parsing ──────────────────────────────────────

/**
 * @brief Parse tool calls from Qwen 3.6 output.
 *
 * Primary path: XML function calls (qwen3_coder format).
 * Fallback: tagged JSON via the base class — covers stray
 * `<tool_call>{...}</tool_call>` outputs.
 *
 * @param content Raw model output.
 * @return ParseResult.
 * @internal
 * @version 2.1.9
 */
ParseResult Qwen36Adapter::parse_tool_calls(const std::string& content) const {
    ParseResult result;

    auto calls = parse_xml_function_calls(content);
    if (calls.empty()) {
        calls = parse_tagged_tool_calls(content);
    }

    result.tool_calls = std::move(calls);
    result.cleaned_content = clean_content(content);
    return result;
}

/**
 * @brief Parse XML function-call blocks.
 *
 * Format: `<function=name><parameter=key>value</parameter></function>`.
 *
 * @param content Model output.
 * @return Vector of parsed tool calls.
 * @internal
 * @version 2.1.9
 */
std::vector<ToolCall> Qwen36Adapter::parse_xml_function_calls(
    const std::string& content) const
{
    std::vector<ToolCall> calls;
    std::regex func_pattern(R"(<function=([^>]+)>([\s\S]*?)</function>)");

    auto begin = std::sregex_iterator(content.begin(), content.end(), func_pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string func_name = (*it)[1].str();
        std::string func_body = (*it)[2].str();

        auto ns = func_name.find_first_not_of(" \t");
        auto ne = func_name.find_last_not_of(" \t");
        if (ns != std::string::npos) {
            func_name = func_name.substr(ns, ne - ns + 1);
        }

        ToolCall tc;
        tc.id = generate_uuid();
        tc.name = func_name;
        tc.arguments = extract_xml_parameters(func_body);
        calls.push_back(std::move(tc));

        logger->info("Parsed XML tool call: {}", func_name);
    }
    return calls;
}

/**
 * @brief Extract `<parameter=...>...</parameter>` pairs from a function body.
 *
 * gh#79 (v2.4.1): delegates to the shared parser which tolerates
 * `</NAME>` close tags in addition to the literal `</parameter>`.
 *
 * @param func_body Function body text.
 * @return Map of parameter key -> value.
 * @internal
 * @version 2.4.1
 */
std::unordered_map<std::string, std::string> Qwen36Adapter::extract_xml_parameters(
    const std::string& func_body) const
{
    return entropic::inference::adapters::parse_xml_parameters(
        func_body, logger);
}

// ── Tool result formatting ─────────────────────────────────

/**
 * @brief Wrap tool result in `<tool_response>` tags as a user message.
 * @param tool_call Executed tool call (name available for logging only).
 * @param result Execution result text.
 * @return User-role message with the wrapped result.
 * @internal
 * @version 2.1.9
 */
Message Qwen36Adapter::format_tool_result(
    const ToolCall& /*tool_call*/,
    const std::string& result) const
{
    Message msg;
    msg.role = "user";
    msg.content = "<tool_response>\n" + result +
                  "\n</tool_response>\n\n" + TOOL_RESULT_SUFFIX;
    return msg;
}

// ── Tool definition formatting ─────────────────────────────

/**
 * @brief Format tool definitions as a `<tools>` JSON array section.
 *
 * Matches Qwen's qwen3_coder template expectations: a `# Tools` header,
 * an OpenAI-function-formatted JSON array inside `<tools>` tags, then
 * the call-format reminder.
 *
 * @param tool_jsons Tool definition JSON strings.
 * @return Section to inject into the system prompt.
 * @internal
 * @version 2.1.9
 */
std::string Qwen36Adapter::format_tools(
    const std::vector<std::string>& tool_jsons) const
{
    nlohmann::json tool_defs = nlohmann::json::array();
    for (const auto& json_str : tool_jsons) {
        try {
            auto j = nlohmann::json::parse(json_str);
            tool_defs.push_back({
                {"type", "function"},
                {"function", {
                    {"name", j.value("name", "unknown")},
                    {"description", j.value("description", "")},
                    {"parameters", j.value("inputSchema",
                        nlohmann::json::object())}
                }}
            });
        } catch (...) {
            continue;
        }
    }

    std::ostringstream out;
    out << "# Tools\n\n"
        << "You may call one or more functions to assist with the user query.\n"
        << "Put your final answer OUTSIDE of any tool calls.\n\n"
        << "Here are the available tools:\n"
        << "<tools>\n"
        << tool_defs.dump(2) << "\n"
        << "</tools>\n\n"
        << "For each function call, return within <tool_call></tool_call> XML tags:\n"
        << "<tool_call>\n"
        << "<function=example_function>\n"
        << "<parameter=param_name>value</parameter>\n"
        << "</function>\n"
        << "</tool_call>";
    return out.str();
}

// ── Content cleaning ───────────────────────────────────────

/**
 * @brief Remove tool calls and think blocks from content.
 * @param content Raw model output.
 * @return Cleaned content.
 * @internal
 * @version 2.1.9
 */
std::string Qwen36Adapter::clean_content(const std::string& content) const {
    std::string cleaned = std::regex_replace(content,
        std::regex(R"(<tool_call>\s*[\s\S]*?\s*</tool_call>)"), "");
    cleaned = std::regex_replace(cleaned,
        std::regex(R"(<function=[^>]+>[\s\S]*?</function>)"), "");
    cleaned = strip_think_blocks(cleaned);
    return cleaned;
}

// ── Vision / multimodal ────────────────────────────────────

/**
 * @brief Append vision instructions to the system prompt when active.
 * @param base_system Base system prompt text.
 * @param has_vision Whether vision is enabled for the tier.
 * @return System prompt, with vision instructions appended if active.
 * @internal
 * @version 2.1.9
 */
std::string Qwen36Adapter::format_system_with_vision(
    const std::string& base_system,
    bool has_vision) const {
    if (!has_vision) {
        return base_system;
    }
    return base_system + VISION_INSTRUCTION;
}

/**
 * @brief Format content parts using the OpenAI content-array convention.
 * @param parts Content parts from a message.
 * @return JSON string in the OpenAI content-array format.
 * @internal
 * @version 2.1.9
 */
std::string Qwen36Adapter::format_content_parts(
    const std::vector<ContentPart>& parts) const {
    return ChatAdapter::format_content_parts(parts);
}

} // namespace entropic
