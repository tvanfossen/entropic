/**
 * @file qwen35_adapter.cpp
 * @brief Qwen 3.5 adapter implementation.
 *
 * XML tool call parsing, <tools> tag formatting, <tool_response> wrapping.
 * Mirrors Python Qwen35Adapter behavior exactly.
 *
 * @version 1.8.2
 */

#include "qwen35_adapter.h"

#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <regex>
#include <sstream>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.adapter.qwen35");

/// @brief Shared continuation text for tool results.
constexpr const char* TOOL_RESULT_SUFFIX =
    "Continue. Batch multiple tool calls in one response when possible.";

/**
 * @brief Generate a simple counter-based ID.
 * @return ID string.
 * @internal
 * @version 1.8.2
 */
std::string generate_uuid() {
    static std::atomic<uint64_t> counter{0};
    return "tc-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

} // anonymous namespace

// ── Tool call parsing ──────────────────────────────────────

/**
 * @brief Parse tool calls from Qwen 3.5 output.
 *
 * Primary: XML function calls. Fallback: tagged JSON (base class).
 *
 * @param content Raw model output.
 * @return ParseResult.
 * @internal
 * @version 1.8.2
 */
ParseResult Qwen35Adapter::parse_tool_calls(const std::string& content) const {
    ParseResult result;

    // Primary: XML function calls
    auto calls = parse_xml_function_calls(content);

    // Fallback: tagged JSON (base class)
    if (calls.empty()) {
        calls = parse_tagged_tool_calls(content);
    }

    result.tool_calls = std::move(calls);
    result.cleaned_content = clean_content(content);
    return result;
}

/**
 * @brief Parse XML-style function calls.
 *
 * Format: <function=name><parameter=key>value</parameter></function>
 *
 * @param content Model output.
 * @return Vector of parsed tool calls.
 * @internal
 * @version 1.8.2
 */
std::vector<ToolCall> Qwen35Adapter::parse_xml_function_calls(
    const std::string& content) const
{
    std::vector<ToolCall> calls;
    std::regex func_pattern(R"(<function=([^>]+)>([\s\S]*?)</function>)");

    auto begin = std::sregex_iterator(content.begin(), content.end(), func_pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string func_name = (*it)[1].str();
        std::string func_body = (*it)[2].str();

        // Trim whitespace from function name
        auto ns = func_name.find_first_not_of(" \t");
        auto ne = func_name.find_last_not_of(" \t");
        if (ns != std::string::npos) {
            func_name = func_name.substr(ns, ne - ns + 1);
        }

        auto arguments = extract_xml_parameters(func_body);

        ToolCall tc;
        tc.id = generate_uuid();
        tc.name = func_name;
        tc.arguments = std::move(arguments);
        calls.push_back(std::move(tc));

        logger->info("Parsed XML tool call: {}", func_name);
    }
    return calls;
}

/**
 * @brief Extract parameter key-value pairs from XML function body.
 *
 * Truncates at nested <function= tag (malformed multi-call output).
 *
 * @param func_body Function body text.
 * @return Map of parameter key → value.
 * @internal
 * @version 1.8.2
 */
std::unordered_map<std::string, std::string> Qwen35Adapter::extract_xml_parameters(
    const std::string& func_body) const
{
    std::string body = func_body;

    // Truncate at nested function start
    auto nested = body.find("<function=");
    if (nested != std::string::npos) {
        logger->warn("Truncating function body at nested <function= tag");
        body = body.substr(0, nested);
    }

    std::unordered_map<std::string, std::string> arguments;
    std::regex param_pattern(R"(<parameter=([^>]+)>([\s\S]*?)</parameter>)");

    auto begin = std::sregex_iterator(body.begin(), body.end(), param_pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string key = (*it)[1].str();
        std::string value = (*it)[2].str();

        // Trim
        auto ks = key.find_first_not_of(" \t");
        auto ke = key.find_last_not_of(" \t");
        if (ks != std::string::npos) key = key.substr(ks, ke - ks + 1);

        auto vs = value.find_first_not_of(" \t");
        auto ve = value.find_last_not_of(" \t");
        if (vs != std::string::npos) value = value.substr(vs, ve - vs + 1);

        if (key.empty() || value.empty()) {
            logger->warn("Skipping empty XML parameter: key='{}' value='{}'", key, value);
            continue;
        }
        arguments[key] = value;
    }
    return arguments;
}

// ── Tool result formatting ─────────────────────────────────

/**
 * @brief Format tool result with <tool_response> tags.
 * @param tool_call Executed tool call.
 * @param result Execution result.
 * @return Formatted user message.
 * @internal
 * @version 1.8.2
 */
Message Qwen35Adapter::format_tool_result(
    const ToolCall& tool_call,
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
 * @brief Format tools in <tools> tags with OpenAI function JSON.
 * @param tool_jsons Tool definition JSON strings.
 * @return Formatted tools section.
 * @internal
 * @version 1.8.2
 */
std::string Qwen35Adapter::format_tools(
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
 * @version 1.8.2
 */
std::string Qwen35Adapter::clean_content(const std::string& content) const {
    // Remove <tool_call>...</tool_call> blocks
    std::string cleaned = std::regex_replace(content,
        std::regex(R"(<tool_call>\s*[\s\S]*?\s*</tool_call>)"), "");

    // Remove standalone <function=...>...</function>
    cleaned = std::regex_replace(cleaned,
        std::regex(R"(<function=[^>]+>[\s\S]*?</function>)"), "");

    // Strip think blocks
    cleaned = strip_think_blocks(cleaned);
    return cleaned;
}

// ── Vision / multimodal (v1.9.11) ──────────────────────────

/// @brief Vision instruction appended to system prompt.
static constexpr const char* VISION_INSTRUCTION =
    "\n\nYou can see and analyze images. When the user shares an "
    "image, describe what you observe before responding to their "
    "question.";

/**
 * @brief Qwen3.5 vision system prompt extension.
 * @param base_system Base system prompt text.
 * @param has_vision Whether the model has vision capability.
 * @return System prompt, with vision instructions appended if active.
 * @internal
 * @version 1.9.11
 */
std::string Qwen35Adapter::format_system_with_vision(
    const std::string& base_system,
    bool has_vision) const {
    if (!has_vision) {
        return base_system;
    }
    return base_system + VISION_INSTRUCTION;
}

/**
 * @brief Qwen3.5 content part formatting (OpenAI-native).
 * @param parts Content parts from a message.
 * @return JSON string in OpenAI content array format.
 * @internal
 * @version 1.9.11
 */
std::string Qwen35Adapter::format_content_parts(
    const std::vector<ContentPart>& parts) const {
    // Qwen3.5 uses OpenAI content array format natively —
    // delegate to base class default.
    return ChatAdapter::format_content_parts(parts);
}

} // namespace entropic
