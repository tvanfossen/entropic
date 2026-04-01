/**
 * @file adapter_base.cpp
 * @brief ChatAdapter base class implementation.
 *
 * Provides shared tool-call parsing, think-block handling, JSON
 * recovery, system prompt assembly, and response completeness detection.
 *
 * @version 1.8.2
 */

#include <entropic/inference/adapters/adapter_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <regex>
#include <sstream>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.adapter");

/// @brief Shared continuation text for tool results.
constexpr const char* TOOL_RESULT_SUFFIX =
    "Continue. Batch multiple tool calls in one response when possible.";

/**
 * @brief Generate a UUID v4 string.
 * @return UUID string.
 * @internal
 * @version 1.8.2
 */
std::string generate_uuid() {
    // Simple counter-based ID for now. Replace with proper UUID if needed.
    static std::atomic<uint64_t> counter{0};
    return "tc-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

} // anonymous namespace

// ── Constructor ────────────────────────────────────────────

/**
 * @brief Construct adapter with tier identity.
 * @param tier_name Identity tier name.
 * @param identity_prompt Assembled identity prompt.
 * @version 1.8.2
 */
ChatAdapter::ChatAdapter(std::string tier_name, std::string identity_prompt)
    : tier_name_(std::move(tier_name))
    , identity_prompt_(std::move(identity_prompt))
{
}

// ── System prompt assembly ─────────────────────────────────

/**
 * @brief Assemble system prompt: identity + context + tools.
 * @param base_prompt Application context.
 * @param tool_jsons Tool definitions as JSON strings.
 * @return Assembled system prompt string.
 * @internal
 * @version 1.8.2
 */
std::string ChatAdapter::format_system_prompt(
    const std::string& base_prompt,
    const std::vector<std::string>& tool_jsons) const
{
    std::string prompt = identity_prompt_;

    if (!base_prompt.empty()) {
        prompt += "\n\n" + base_prompt;
    }

    if (!tool_jsons.empty()) {
        // Extract tool prefixes for later parsing
        for (const auto& json_str : tool_jsons) {
            try {
                auto j = nlohmann::json::parse(json_str);
                std::string name = j.value("name", "");
                auto dot = name.find('.');
                if (dot != std::string::npos) {
                    tool_prefixes_.insert(name.substr(0, dot));
                }
            } catch (...) {
                // Skip malformed tool JSON
            }
        }
        prompt += "\n\n" + format_tools(tool_jsons);
    }

    return prompt;
}

// ── Tool result formatting ─────────────────────────────────

/**
 * @brief Format tool result as user message (default).
 * @param tool_call The executed tool call.
 * @param result Execution result text.
 * @return Formatted user message.
 * @internal
 * @version 1.8.2
 */
Message ChatAdapter::format_tool_result(
    const ToolCall& tool_call,
    const std::string& result) const
{
    Message msg;
    msg.role = "user";
    msg.content = "Tool `" + tool_call.name + "` returned:\n\n" +
                  result + "\n\n" + TOOL_RESULT_SUFFIX;
    return msg;
}

// ── Response completeness ──────────────────────────────────

/**
 * @brief Check if response represents task completion.
 *
 * Think-aware: think-only content without tool calls = still working.
 *
 * @param content Response content.
 * @param tool_calls Parsed tool calls.
 * @return true if complete.
 * @internal
 * @version 1.8.2
 */
bool ChatAdapter::is_response_complete(
    const std::string& content,
    const std::vector<ToolCall>& tool_calls) const
{
    // Has tool calls → not complete (needs execution)
    if (!tool_calls.empty()) {
        return false;
    }

    // Unclosed think block → still thinking
    if (content.find("<think>") != std::string::npos &&
        content.find("</think>") == std::string::npos)
    {
        return false;
    }

    // Strip think blocks and check for real content
    std::string stripped = strip_think_blocks(content);
    return !stripped.empty();
}

// ── Tagged tool call parsing ───────────────────────────────

/**
 * @brief Parse <tool_call>JSON</tool_call> tagged blocks.
 * @param content Model output content.
 * @return Vector of parsed tool calls.
 * @internal
 * @version 1.8.2
 */
std::vector<ToolCall> ChatAdapter::parse_tagged_tool_calls(
    const std::string& content) const
{
    std::vector<ToolCall> calls;
    std::regex pattern(R"(<tool_call>\s*([\s\S]*?)\s*</tool_call>)");

    auto begin = std::sregex_iterator(content.begin(), content.end(), pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string json_str = (*it)[1].str();
        auto parsed = parse_single_tool_call(json_str);
        if (parsed) {
            calls.push_back(*parsed);
            logger->info("Parsed tagged tool call: {}", parsed->name);
        }
    }
    return calls;
}

// ── Bare JSON parsing ──────────────────────────────────────

/**
 * @brief Parse bare JSON tool calls from lines.
 * @param content Model output.
 * @return Vector of tool calls from bare JSON lines.
 * @internal
 * @version 1.8.3
 */
std::vector<ToolCall> ChatAdapter::parse_bare_json_tool_calls(
    const std::string& content) const
{
    std::vector<ToolCall> calls;
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        // Trim
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string_view stripped(line.data() + start, line.size() - start);

        if (stripped.front() != '{' || stripped.find("\"name\"") == std::string_view::npos) {
            continue;
        }

        try {
            auto j = nlohmann::json::parse(stripped);
            if (j.contains("name")) {
                ToolCall tc;
                tc.id = generate_uuid();
                tc.name = j["name"].get<std::string>();
                auto args = j.value("arguments", j.value("parameters", nlohmann::json::object()));
                for (auto& [k, v] : args.items()) {
                    tc.arguments[k] = v.dump();
                }
                calls.push_back(tc);
            }
        } catch (...) {
            // Skip unparseable lines
        }
    }
    return calls;
}

// ── Think block handling ───────────────────────────────────

/**
 * @brief Extract content from <think>...</think> blocks.
 * @param content Model output.
 * @return Concatenated thinking content, or empty string.
 * @internal
 * @version 1.8.2
 */
std::string ChatAdapter::extract_thinking(const std::string& content) const {
    std::string result;
    std::regex pattern(R"(<think>([\s\S]*?)</think>)");

    auto begin = std::sregex_iterator(content.begin(), content.end(), pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        if (!result.empty()) result += "\n";
        result += (*it)[1].str();
    }
    return result;
}

/**
 * @brief Strip all <think>...</think> blocks from content.
 * @param content Model output.
 * @return Content with think blocks removed.
 * @internal
 * @version 1.8.2
 */
std::string ChatAdapter::strip_think_blocks(const std::string& content) const {
    std::regex pattern(R"(<think>[\s\S]*?</think>)");
    std::string result = std::regex_replace(content, pattern, "");

    // Trim
    size_t start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end_pos = result.find_last_not_of(" \t\n\r");
    return result.substr(start, end_pos - start + 1);
}

// ── JSON recovery ──────────────────────────────────────────

/**
 * @brief Attempt JSON recovery on malformed tool call string.
 *
 * Tries: trailing comma removal, quote normalization, brace matching.
 *
 * @param json_str Potentially malformed JSON.
 * @return Recovered ToolCall if successful.
 * @internal
 * @version 1.8.2
 */
std::optional<ToolCall> ChatAdapter::try_recover_json(
    const std::string& json_str) const
{
    // Fix trailing commas and single quotes
    std::string fixed = std::regex_replace(json_str, std::regex(R"(,\s*\})"), "}");
    fixed = std::regex_replace(fixed, std::regex(R"(,\s*\])"), "]");
    std::replace(fixed.begin(), fixed.end(), '\'', '"');

    try {
        auto j = nlohmann::json::parse(fixed);
        if (j.contains("name")) {
            ToolCall tc;
            tc.id = generate_uuid();
            tc.name = j["name"].get<std::string>();
            auto args = j.value("arguments", j.value("parameters", nlohmann::json::object()));
            for (auto& [k, v] : args.items()) {
                tc.arguments[k] = v.dump();
            }
            return tc;
        }
    } catch (...) {
        // Final fallback: extract name via regex
        std::regex name_pattern(R"re("name"\s*:\s*"([^"]+)")re");
        std::smatch match;
        if (std::regex_search(json_str, match, name_pattern)) {
            ToolCall tc;
            tc.id = generate_uuid();
            tc.name = match[1].str();
            return tc;
        }
    }
    return std::nullopt;
}

// ── Tool formatting (default) ──────────────────────────────

/**
 * @brief Default tool formatting: markdown headings + JSON schema.
 * @param tool_jsons Tool definitions as JSON strings.
 * @return Formatted tool section string.
 * @internal
 * @version 1.8.2
 */
std::string ChatAdapter::format_tools(
    const std::vector<std::string>& tool_jsons) const
{
    std::ostringstream out;
    out << "## Tools\n\n"
        << "Call tools with: `<tool_call>{\"name\": \"tool.name\", \"arguments\": {...}}</tool_call>`\n"
        << "Batch independent calls in one response with multiple `<tool_call>` blocks.\n\n";

    for (const auto& json_str : tool_jsons) {
        try {
            auto j = nlohmann::json::parse(json_str);
            out << "### " << j.value("name", "unknown") << "\n"
                << j.value("description", "No description") << "\n\n"
                << "Schema:\n```json\n"
                << j.value("inputSchema", nlohmann::json::object()).dump(2)
                << "\n```\n\n";
        } catch (...) {
            out << "### (malformed tool definition)\n\n";
        }
    }
    return out.str();
}

// ── Internal helper ────────────────────────────────────────

/**
 * @brief Parse a single JSON tool call string.
 * @param json_str JSON string from tagged block.
 * @return Parsed ToolCall or nullopt.
 * @internal
 * @version 1.8.2
 */
std::optional<ToolCall> ChatAdapter::parse_single_tool_call(
    const std::string& json_str) const
{
    try {
        auto j = nlohmann::json::parse(json_str);
        if (j.contains("name")) {
            ToolCall tc;
            tc.id = generate_uuid();
            tc.name = j["name"].get<std::string>();
            auto args = j.value("arguments", j.value("parameters", nlohmann::json::object()));
            for (auto& [k, v] : args.items()) {
                tc.arguments[k] = v.dump();
            }
            return tc;
        }
    } catch (...) {
        return try_recover_json(json_str);
    }
    return std::nullopt;
}

// ── Vision / multimodal (v1.9.11) ──────────────────────────

/**
 * @brief Default: return system prompt unchanged regardless of vision.
 * @param base_system Base system prompt text.
 * @param has_vision Whether vision is available.
 * @return base_system unchanged.
 * @internal
 * @version 1.9.11
 */
std::string ChatAdapter::format_system_with_vision(
    const std::string& base_system,
    bool /*has_vision*/) const {
    return base_system;
}

/**
 * @brief Default: OpenAI-format content array JSON.
 * @param parts Content parts from a message.
 * @return JSON array string with text/image objects.
 * @internal
 * @version 1.9.11
 */
std::string ChatAdapter::format_content_parts(
    const std::vector<ContentPart>& parts) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& part : parts) {
        nlohmann::json obj;
        if (part.type == ContentPartType::TEXT) {
            obj["type"] = "text";
            obj["text"] = part.text;
        } else {
            obj["type"] = "image";
            if (!part.image_path.empty()) {
                obj["path"] = part.image_path;
            }
            if (!part.image_url.empty()) {
                obj["url"] = part.image_url;
            }
        }
        arr.push_back(std::move(obj));
    }
    return arr.dump();
}

} // namespace entropic
