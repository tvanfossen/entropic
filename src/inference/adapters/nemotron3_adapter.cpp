// SPDX-License-Identifier: Apache-2.0
/**
 * @file nemotron3_adapter.cpp
 * @brief Nemotron 3 adapter implementation (v2.1.9, gh#47; v2.3.8, gh#70).
 *
 * Tool-call parsing targets the DSML invoke format the `nemotron_h`
 * GGUFs actually emit (gh#70), with the qwen3_coder XML and tagged-JSON
 * paths retained as backstops. The chat template itself is GGUF-embedded
 * so `chat_format()` returns an empty string and llama.cpp drives the
 * template application.
 *
 * @version 2.3.8
 */

#include "nemotron3_adapter.h"

#include "xml_parameter_parser.h"

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
 * @brief Parse tool calls from Nemotron 3 output.
 *
 * Layered strategy (gh#70): DSML invoke first (the native emit), then
 * the qwen3_coder XML path and the tagged-JSON path as backstops for
 * rigged-prompt or mixed-format consumers.
 *
 * @param content Raw model output.
 * @return ParseResult.
 * @internal
 * @version 2.3.8
 */
ParseResult Nemotron3Adapter::parse_tool_calls(const std::string& content) const {
    ParseResult result;

    auto calls = parse_dsml_function_calls(content);
    if (calls.empty()) {
        calls = parse_xml_function_calls(content);
    }
    if (calls.empty()) {
        calls = parse_tagged_tool_calls(content);
    }

    result.tool_calls = std::move(calls);
    result.cleaned_content = clean_content(content);
    return result;
}

// ── DSML invoke parsing (gh#70, native Nemotron 3 format) ──

/**
 * @brief Parse `<｜DSML｜invoke name="X">...</｜DSML｜invoke>` blocks.
 *
 * Fullwidth-pipe `｜` (U+FF5C) is matched as its literal UTF-8 byte
 * sequence. Each invoke block is parsed independently of the optional
 * `<｜DSML｜function_calls>` wrapper.
 *
 * @param content Model output.
 * @return Vector of parsed tool calls.
 * @internal
 * @version 2.3.8
 */
std::vector<ToolCall> Nemotron3Adapter::parse_dsml_function_calls(
    const std::string& content) const
{
    std::vector<ToolCall> calls;
    // Custom `re` delimiter: the pattern contains `)"` sequences (from
    // the `name="..."` capture) that would terminate a default R"(...)"
    // literal early.
    std::regex invoke_pattern(
        R"re(<｜DSML｜invoke name="([^"]+)">([\s\S]*?)</｜DSML｜invoke>)re");

    auto begin = std::sregex_iterator(content.begin(), content.end(), invoke_pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        ToolCall tc;
        tc.id = generate_uuid();
        tc.name = (*it)[1].str();
        tc.arguments = extract_dsml_parameters((*it)[2].str());
        calls.push_back(std::move(tc));

        logger->info("Parsed DSML invoke: {}", calls.back().name);
    }
    return calls;
}

/**
 * @brief Extract `<｜DSML｜parameter name="K" string="V"/>` pairs.
 *
 * Accepts the `string` / `int` / `bool` / `value` typed attribute
 * keyword; the value is stored verbatim (no JSON quoting).
 *
 * @param invoke_body Invoke block body text.
 * @return Map of parameter key -> value.
 * @internal
 * @version 2.3.8
 */
std::unordered_map<std::string, std::string> Nemotron3Adapter::extract_dsml_parameters(
    const std::string& invoke_body) const
{
    std::unordered_map<std::string, std::string> arguments;
    // Custom `re` delimiter — same `)"`-in-pattern reason as the
    // invoke regex above.
    std::regex param_pattern(
        R"re(<｜DSML｜parameter name="([^"]+)"\s+(?:string|int|bool|value)="([^"]*)"\s*/>)re");

    auto begin = std::sregex_iterator(invoke_body.begin(), invoke_body.end(), param_pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        arguments[(*it)[1].str()] = (*it)[2].str();
    }
    return arguments;
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
 *
 * gh#79 (v2.4.1): delegates to the shared parser which tolerates
 * `</NAME>` close tags in addition to the literal `</parameter>`.
 *
 * @param func_body Function body text.
 * @return Map of parameter key -> value.
 * @internal
 * @version 2.4.1
 */
std::unordered_map<std::string, std::string> Nemotron3Adapter::extract_xml_parameters(
    const std::string& func_body) const
{
    return entropic::inference::adapters::parse_xml_parameters(
        func_body, logger);
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
 * @brief Build the OpenAI-function `<tools>` JSON array for injection.
 * @param tool_jsons Tool definition JSON strings.
 * @return JSON array of `{type, function:{name,description,parameters}}`.
 * @internal
 * @version 2.3.8
 */
static nlohmann::json build_tool_defs(
    const std::vector<std::string>& tool_jsons)
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
    return tool_defs;
}

/**
 * @brief Format tool definitions and teach the DSML invoke call format.
 *
 * gh#70: the injected call-format example matches what the `nemotron_h`
 * GGUFs actually emit (DSML invoke, fullwidth-pipe `｜`), instead of the
 * qwen XML the model never produced under the production prompt.
 *
 * @param tool_jsons Tool definition JSON strings.
 * @return Section to inject into the system prompt.
 * @internal
 * @version 2.3.8
 */
std::string Nemotron3Adapter::format_tools(
    const std::vector<std::string>& tool_jsons) const
{
    std::ostringstream out;
    out << "# Tools\n\n"
        << "You may call one or more functions to assist with the user query.\n"
        << "Put your final answer OUTSIDE of any tool calls.\n\n"
        << "Here are the available tools:\n"
        << "<tools>\n"
        << build_tool_defs(tool_jsons).dump(2) << "\n"
        << "</tools>\n\n"
        << "For each function call, emit a DSML invoke block:\n"
        << "<｜DSML｜function_calls>\n"
        << "<｜DSML｜invoke name=\"example.tool\">\n"
        << "<｜DSML｜parameter name=\"param_name\" string=\"value\"/>\n"
        << "</｜DSML｜invoke>\n"
        << "</｜DSML｜function_calls>";
    return out.str();
}

// ── Content cleaning ───────────────────────────────────────

/**
 * @brief Remove tool calls, DSML markup, and `<think>` blocks from content.
 *
 * gh#70 scrubs three DSML layers so the assistant-visible body never
 * carries fullwidth-pipe (`｜`, U+FF5C) channel artifacts:
 *   1. the `<｜DSML｜function_calls>...</｜DSML｜function_calls>` wrapper,
 *   2. any bare `<｜DSML｜invoke ...>...</｜DSML｜invoke>` block, and
 *   3. any remaining `<｜...｜>`-style channel token (e.g. the
 *      `<｜begin▁of▁sentence｜>` BOS spam seen at Q8 / Q4_K_XL).
 * The qwen XML / tagged-JSON scrubs are kept as backstops.
 *
 * @param content Raw model output.
 * @return Cleaned content.
 * @internal
 * @version 2.3.8
 */
std::string Nemotron3Adapter::clean_content(const std::string& content) const {
    std::string cleaned = std::regex_replace(content,
        std::regex(R"(<tool_call>\s*[\s\S]*?\s*</tool_call>)"), "");
    cleaned = std::regex_replace(cleaned,
        std::regex(R"(<function=[^>]+>[\s\S]*?</function>)"), "");
    cleaned = std::regex_replace(cleaned,
        std::regex(R"(<｜DSML｜function_calls>[\s\S]*?</｜DSML｜function_calls>)"), "");
    cleaned = std::regex_replace(cleaned,
        std::regex(R"(<｜DSML｜invoke[\s\S]*?</｜DSML｜invoke>)"), "");
    cleaned = std::regex_replace(cleaned,
        std::regex(R"(</?｜[^>]*>)"), "");
    cleaned = strip_think_blocks(cleaned);
    return cleaned;
}

} // namespace entropic
