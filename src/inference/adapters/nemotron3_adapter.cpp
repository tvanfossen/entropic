// SPDX-License-Identifier: Apache-2.0
/**
 * @file nemotron3_adapter.cpp
 * @brief Nemotron 3 adapter implementation (v2.1.9, gh#47).
 *
 * Tool-call parsing mirrors qwen3_coder XML format; the chat template
 * itself is GGUF-embedded so `chat_format()` returns an empty string
 * and llama.cpp drives the template application.
 *
 * @version 2.1.9
 */

#include "nemotron3_adapter.h"

#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <regex>
#include <sstream>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.adapter.nemotron3");

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

} // anonymous namespace

// ── Tool call parsing ──────────────────────────────────────

/**
 * @brief Parse tool calls from Nemotron 3 output (qwen3_coder XML).
 * @param content Raw model output.
 * @return ParseResult.
 * @internal
 * @version 2.1.9
 */
ParseResult Nemotron3Adapter::parse_tool_calls(const std::string& content) const {
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
 * @brief Parse `<function=name><parameter=key>value</parameter></function>` blocks.
 * @param content Model output.
 * @return Vector of parsed tool calls.
 * @internal
 * @version 2.1.9
 */
std::vector<ToolCall> Nemotron3Adapter::parse_xml_function_calls(
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
 * @param func_body Function body text.
 * @return Map of parameter key -> value.
 * @internal
 * @version 2.1.9
 */
std::unordered_map<std::string, std::string> Nemotron3Adapter::extract_xml_parameters(
    const std::string& func_body) const
{
    std::string body = func_body;
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

        auto ks = key.find_first_not_of(" \t\n\r");
        auto ke = key.find_last_not_of(" \t\n\r");
        if (ks != std::string::npos) key = key.substr(ks, ke - ks + 1);

        auto vs = value.find_first_not_of(" \t\n\r");
        auto ve = value.find_last_not_of(" \t\n\r");
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
 * @brief Wrap tool result in `<tool_response>` tags as a user message.
 * @param tool_call Executed tool call (used for logging only).
 * @param result Execution result text.
 * @return User-role message with the wrapped result.
 * @internal
 * @version 2.1.9
 */
Message Nemotron3Adapter::format_tool_result(
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
 * Mirrors the Qwen qwen3_coder convention since Nemotron 3 uses the
 * same parser upstream.
 *
 * @param tool_jsons Tool definition JSON strings.
 * @return Section to inject into the system prompt.
 * @internal
 * @version 2.1.9
 */
std::string Nemotron3Adapter::format_tools(
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
 * @brief Remove tool calls and `<think>` blocks from content.
 * @param content Raw model output.
 * @return Cleaned content.
 * @internal
 * @version 2.1.9
 */
std::string Nemotron3Adapter::clean_content(const std::string& content) const {
    std::string cleaned = std::regex_replace(content,
        std::regex(R"(<tool_call>\s*[\s\S]*?\s*</tool_call>)"), "");
    cleaned = std::regex_replace(cleaned,
        std::regex(R"(<function=[^>]+>[\s\S]*?</function>)"), "");
    cleaned = strip_think_blocks(cleaned);
    return cleaned;
}

} // namespace entropic
