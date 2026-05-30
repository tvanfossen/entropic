// SPDX-License-Identifier: Apache-2.0
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
#include <unordered_set>

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

/**
 * @brief Extract a tool name from a JSON call object, accepting aliases.
 *
 * gh#71-phase-2: weaker models (e.g. Gemma 4 E2B) emit the tool name
 * under `tool_name` / `function` / `function_name` instead of the
 * canonical `name`. These are well-formed calls in everything but the
 * key spelling — accept the common aliases rather than drop the call.
 * A call with no name key under any alias is genuinely unparseable
 * (the function is unknown) and yields an empty string.
 *
 * @param j Parsed JSON tool-call object.
 * @return The tool name, or empty string if no name key is present.
 * @internal
 * @version 2.3.8
 */
std::string tool_name_from_json(const nlohmann::json& j) {
    for (const char* key : {"name", "tool_name", "function", "function_name"}) {
        if (j.contains(key) && j[key].is_string()) {
            return j[key].get<std::string>();
        }
    }
    return "";
}

/**
 * @brief Build a ToolCall from a parsed JSON object (name + args), or nullopt.
 *
 * Single source of truth for JSON-object -> ToolCall conversion, shared
 * by the tagged, bare-JSON, and recovery paths so the name-alias and
 * arguments/parameters handling stay consistent. Returns nullopt when no
 * tool name is present under any accepted alias.
 *
 * @param j Parsed JSON tool-call object.
 * @return Populated ToolCall, or nullopt if nameless.
 * @internal
 * @version 2.3.8
 */
std::optional<ToolCall> tool_call_from_json(const nlohmann::json& j) {
    std::string name = tool_name_from_json(j);
    if (name.empty()) { return std::nullopt; }
    ToolCall tc;
    tc.id = generate_uuid();
    tc.name = std::move(name);
    auto args = j.value("arguments", j.value("parameters", nlohmann::json::object()));
    for (auto& [k, v] : args.items()) { tc.arguments[k] = v.dump(); }
    return tc;
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
 * @version 2.3.8
 */
std::vector<ToolCall> ChatAdapter::parse_tagged_tool_calls(
    const std::string& content) const
{
    std::vector<ToolCall> calls;
    // gh#65 (v2.3.3): accept asymmetric open tags. Gemma 4 emits
    // `<|tool_call>` (pipe-prefixed open, plain close) — the special
    // token `<|tool_call|>` decodes through llama.cpp's current pin
    // as `<|tool_call>` (trailing `|>` lost). Pre-v2.3.3 the regex
    // required a plain `<tool_call>` open, so Gemma 4's actual output
    // produced 0 tool calls and the engine looped on the retry banner.
    //
    // gh#69 (v2.3.8): add `<|im_start|>tool_call` as a fourth open
    // variant. Gemma 4 (E2B + E4B) emits its tool calls inside a
    // ChatML-style channel whose opening header is `<|im_start|>tool_call`
    // but whose close is the plain `</tool_call>` — an asymmetric pair
    // the prior three alternatives didn't cover, so both Gemma 4 sizes
    // scored 0/6 completion (agent loop spiralled to the iteration cap).
    //
    // Open alternatives: `<tool_call>`, `<|tool_call>`, `<|tool_call|>`,
    // `<|im_start|>tool_call`. Close tag stays `</tool_call>` — that's
    // what the consumer's transcripts consistently show.
    std::regex pattern(
        R"((?:<tool_call>|<\|tool_call\|?>|<\|im_start\|>tool_call)\s*)"
        R"(([\s\S]*?)\s*</tool_call>)");

    auto begin = std::sregex_iterator(content.begin(), content.end(), pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string json_str = (*it)[1].str();
        auto parsed = parse_single_tool_call(json_str);
        if (parsed) {
            calls.push_back(*parsed);
            logger->info("Parsed tagged tool call: {}", parsed->name);
        } else {
            // gh#65: when the regex matches but parse_single_tool_call
            // returns nullopt, the JSON payload was malformed in a way
            // try_recover_json could not fix. Log the offending text so
            // future investigations have something to grep, instead of
            // silently producing zero tool calls.
            logger->warn(
                "Tagged tool_call matched but JSON failed to parse: {}",
                json_str);
        }
    }
    // gh#65/gh#69: model emitted tool_call markup but no regex match.
    // Catches plain `<tool_call>`, pipe-prefixed `<|tool_call`, and the
    // Gemma 4 channel header `<|im_start|>tool_call` substrings — if
    // none matched the full pattern, surface the raw content's length so
    // the consumer can attach it for triage instead of seeing a silent
    // "tool_calls: 0".
    if (calls.empty()
        && (content.find("<tool_call>") != std::string::npos
            || content.find("<|tool_call") != std::string::npos
            || content.find("<|im_start|>tool_call") != std::string::npos)) {
        logger->warn(
            "Content contains tool_call markup but no tagged calls "
            "were extracted — possible tag/encoding mismatch. "
            "Raw content length={}", content.size());
    }
    return calls;
}

// ── Bare JSON parsing ──────────────────────────────────────

/**
 * @brief Parse bare JSON tool calls from lines.
 * @param content Model output.
 * @return Vector of tool calls from bare JSON lines.
 * @internal
 * @version 2.3.8
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

        // Gate on a name key under any accepted alias (gh#71-phase-2):
        // a bare `{"name":...}` or `{"tool_name":...}` line is a call.
        if (stripped.front() != '{'
            || (stripped.find("name") == std::string_view::npos)) {
            continue;
        }

        try {
            auto j = nlohmann::json::parse(stripped);
            if (auto tc = tool_call_from_json(j)) {
                calls.push_back(*tc);
            }
        } catch (...) {
            // Skip unparseable lines
        }
    }
    return calls;
}

// ── gh#88 action-envelope recovery (free functions) ─────────

/**
 * @brief Map one parsed JSON object to a recovered ToolCall.
 *
 * Accepts the canonical `{"name":...}` shape (via tool_call_from_json) and
 * the parroted `{"action":"<verb>",...}` meta envelope — the latter mapped
 * to the `entropic.<verb>` call ONLY when <verb> names a known meta-tool
 * (its action verb equals its tool short-name). gh#88 audit: without this
 * allow-list, TodoTool's sub-action verbs (add/update/remove) and arbitrary
 * prose strings were recovered as bogus `entropic.<x>` no-op calls — the
 * recovery would WARN "recovered 1" while injecting a call to a tool that
 * does not exist.
 *
 * @param j Parsed JSON object.
 * @return ToolCall, or nullopt if neither shape matches a real tool.
 * @internal
 * @version 2.7.1
 */
static std::optional<ToolCall> action_envelope_to_call(
    const nlohmann::json& j) {
    if (auto tc = tool_call_from_json(j)) { return tc; }
    // gh#88: meta-tools whose result `action` verb equals their tool name.
    // TodoTool (add/update/remove) and free-text actions are excluded.
    static const std::unordered_set<std::string> kMetaActions = {
        "delegate", "pipeline", "complete",
        "phase_change", "prune_context", "resume_delegation"};
    std::string action;
    if (j.contains("action") && j["action"].is_string()) {
        action = j["action"].get<std::string>();
    }
    if (kMetaActions.find(action) == kMetaActions.end()) {
        return std::nullopt;
    }
    ToolCall tc;
    tc.id = generate_uuid();
    tc.name = "entropic." + action;
    nlohmann::json args = j;
    args.erase("action");
    tc.arguments_json = args.dump();
    for (auto& [k, v] : args.items()) { tc.arguments[k] = v.dump(); }
    return tc;
}

/**
 * @brief gh#88 bare-JSON recovery — see header for the full rationale.
 * @param raw Raw assistant output.
 * @return Recovered tool calls (empty when none match).
 * @utility
 * @version 2.7.1
 */
std::vector<ToolCall> recover_action_envelope_calls(const std::string& raw) {
    std::vector<ToolCall> calls;
    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] != '{') { continue; }
        auto j = nlohmann::json::parse(line.substr(start), nullptr, false);
        if (!j.is_object()) { continue; }
        if (auto tc = action_envelope_to_call(j)) {
            calls.push_back(std::move(*tc));
        }
    }
    return calls;
}

/**
 * @brief gh#88 reliable-path recovery substitution — see header.
 * @param calls In/out parsed calls; replaced by the recovery iff empty + found.
 * @param raw   Raw assistant output to recover from.
 * @utility
 * @version 2.7.1
 */
void apply_action_envelope_recovery(std::vector<ToolCall>& calls,
                                    const std::string& raw) {
    if (!calls.empty()) { return; }
    auto recovered = recover_action_envelope_calls(raw);
    if (recovered.empty()) { return; }
    logger->warn(
        "gh#88: PEG_GEMMA4 parsed 0 tool calls; recovered {} from a "
        "bare-JSON {{\"action\":...}} envelope (possible context priming)",
        recovered.size());
    calls = std::move(recovered);
}

// ── gh#90 string-typed-arg coercion (free functions) ────────

/**
 * @brief String-typed property names for a tool in the staged MCP defs.
 * @param tools Parsed MCP tool array.
 * @param name  Tool name to match.
 * @return Property names whose schema declares `"type":"string"`.
 * @internal
 * @version 2.7.2
 */
static std::unordered_set<std::string> tool_string_props(
    const nlohmann::json& tools, const std::string& name) {
    std::unordered_set<std::string> props;
    for (const auto& t : tools) {
        if (!t.is_object() || t.value("name", std::string()) != name) {
            continue;
        }
        auto schema = t.value("inputSchema", nlohmann::json::object());
        auto properties = schema.value("properties", nlohmann::json::object());
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (it->is_object()
                && it->value("type", std::string()) == "string") {
                props.insert(it.key());
            }
        }
        break;
    }
    return props;
}

/**
 * @brief Coerce one call's numeric args to strings per the tool schema.
 * @param tc    Tool call (arguments_json + arguments mutated in place).
 * @param tools Parsed MCP tool array.
 * @internal
 * @version 2.7.2
 */
static void coerce_call_string_args(ToolCall& tc, const nlohmann::json& tools) {
    auto props = tool_string_props(tools, tc.name);
    if (props.empty()) { return; }
    auto args = nlohmann::json::parse(tc.arguments_json, nullptr, false);
    if (!args.is_object()) { return; }
    bool changed = false;
    for (const auto& key : props) {
        auto it = args.find(key);
        if (it != args.end() && it->is_number()) {
            *it = it->dump();  // JSON number 3 → JSON string "3"
            changed = true;
        }
    }
    if (!changed) { return; }
    tc.arguments_json = args.dump();
    for (auto it = args.begin(); it != args.end(); ++it) {
        tc.arguments[it.key()] =
            it->is_string() ? it->get<std::string>() : it->dump();
    }
}

/**
 * @brief gh#90 string-typing coercion entry point — see header.
 * @param calls      In/out parsed calls; numeric args re-typed in place.
 * @param tools_json Staged MCP tool defs (JSON array) carrying the schema.
 * @utility
 * @version 2.7.2
 */
void coerce_string_typed_args(std::vector<ToolCall>& calls,
                              const std::string& tools_json) {
    if (calls.empty() || tools_json.empty()) { return; }
    auto tools = nlohmann::json::parse(tools_json, nullptr, false);
    if (!tools.is_array()) { return; }
    for (auto& tc : calls) { coerce_call_string_args(tc, tools); }
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
 * @brief Parse a brace/quote-fixed JSON string into a ToolCall.
 * @param fixed Cleaned JSON string (may still be invalid → throws).
 * @return ToolCall if it has a name (any alias), else nullopt.
 * @utility
 * @version 2.3.8
 */
static std::optional<ToolCall> parse_recovered_tool_call(
    const std::string& fixed) {
    auto j = nlohmann::json::parse(fixed);
    return tool_call_from_json(j);
}

/**
 * @brief Last-ditch recovery: pull a tool name out via regex.
 * @param json_str Original (possibly unparseable) string.
 * @return ToolCall with just id+name, or nullopt if no name found.
 * @utility
 * @version 2.3.7
 */
static std::optional<ToolCall> regex_recovered_tool_call(
    const std::string& json_str) {
    std::regex name_pattern(R"re("name"\s*:\s*"([^"]+)")re");
    std::smatch match;
    if (!std::regex_search(json_str, match, name_pattern)) {
        return std::nullopt;
    }
    ToolCall tc;
    tc.id = generate_uuid();
    tc.name = match[1].str();
    return tc;
}

/**
 * @brief Attempt JSON recovery on malformed tool call string.
 *
 * Tries: trailing comma removal, quote normalization, brace matching.
 *
 * @param json_str Potentially malformed JSON.
 * @return Recovered ToolCall if successful.
 * @internal
 * @version 2.3.7
 */
std::optional<ToolCall> ChatAdapter::try_recover_json(
    const std::string& json_str) const
{
    // Fix trailing commas and single quotes
    std::string fixed = std::regex_replace(json_str, std::regex(R"(,\s*\})"), "}");
    fixed = std::regex_replace(fixed, std::regex(R"(,\s*\])"), "]");
    std::replace(fixed.begin(), fixed.end(), '\'', '"');

    logger->info("JSON recovery attempt: {} chars", json_str.size());
    try {
        if (auto tc = parse_recovered_tool_call(fixed)) { return tc; }
    } catch (...) {
        return regex_recovered_tool_call(json_str);
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
 * @version 2.3.8
 */
std::optional<ToolCall> ChatAdapter::parse_single_tool_call(
    const std::string& json_str) const
{
    try {
        auto j = nlohmann::json::parse(json_str);
        if (auto tc = tool_call_from_json(j)) {
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
