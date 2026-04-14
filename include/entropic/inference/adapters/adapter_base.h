/**
 * @file adapter_base.h
 * @brief ChatAdapter concrete base class.
 *
 * @par Responsibilities (80% in base)
 * - System prompt assembly (identity + context + tools)
 * - Think-block extraction and stripping
 * - Tagged tool call parsing (<tool_call>JSON</tool_call>)
 * - Bare JSON tool call parsing (fallback)
 * - Malformed JSON recovery
 * - Tool result formatting
 * - Response completeness detection (think-aware)
 *
 * @par Subclass overrides (20%)
 * - parse_tool_calls() — model-specific parsing strategy
 * - format_tools() — tool definition format
 * - format_tool_result() — if model needs special wrapping
 * - chat_format() — chat format identifier
 *
 * Internal to inference .so — nlohmann/json used internally, not in
 * interface headers.
 *
 * @version 1.8.2
 */

#pragma once

#include <entropic/types/message.h>
#include <entropic/types/tool_call.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace entropic {

/**
 * @brief Parsed tool call result: cleaned content + extracted calls.
 * @version 1.8.2
 */
struct ParseResult {
    std::string cleaned_content;       ///< Content with tool calls removed
    std::vector<ToolCall> tool_calls;  ///< Extracted tool calls
};

/**
 * @brief Concrete base class for chat format adapters (80% logic).
 *
 * Provides shared parsing primitives, think-block handling, JSON
 * recovery, and system prompt assembly. Subclasses override
 * parse_tool_calls() and optionally format_tools()/format_tool_result().
 *
 * @version 1.8.2
 */
class ChatAdapter {
public:
    /**
     * @brief Construct adapter with tier identity.
     * @param tier_name Identity tier (e.g. "eng", "lead").
     * @param identity_prompt Assembled identity prompt.
     * @version 1.8.2
     */
    ChatAdapter(std::string tier_name, std::string identity_prompt);
    virtual ~ChatAdapter() = default;

    /* ── System prompt assembly ──────────────────────────── */

    /**
     * @brief Assemble system prompt: identity + context + tools.
     * @param base_prompt Application context.
     * @param tool_jsons Tool definitions as JSON strings.
     * @return Assembled system prompt.
     * @version 1.8.2
     */
    std::string format_system_prompt(
        const std::string& base_prompt,
        const std::vector<std::string>& tool_jsons) const;

    /* ── Tool call parsing (subclass overrides) ──────────── */

    /**
     * @brief Parse tool calls from model output.
     * @param content Raw model output.
     * @return ParseResult with cleaned content and tool calls.
     * @version 1.8.2
     */
    virtual ParseResult parse_tool_calls(
        const std::string& content) const = 0;

    /* ── Tool result formatting ──────────────────────────── */

    /**
     * @brief Format a tool result as a user message.
     * @param tool_call The tool call that was executed.
     * @param result Tool execution result text.
     * @return Formatted message.
     * @version 1.8.2
     */
    virtual Message format_tool_result(
        const ToolCall& tool_call,
        const std::string& result) const;

    /* ── Tool formatting (subclass can override) ─────────── */

    /**
     * @brief Format tool definitions for injection into system prompt.
     * @param tool_jsons Tool definition JSON strings.
     * @return Adapter-formatted tool prompt string.
     * @version 2.0.4
     */
    virtual std::string format_tools(
        const std::vector<std::string>& tool_jsons) const;

    /* ── Response completeness ───────────────────────────── */

    /**
     * @brief Check if response represents task completion.
     * @param content Response content.
     * @param tool_calls Parsed tool calls.
     * @return true if complete.
     * @version 1.8.2
     */
    bool is_response_complete(
        const std::string& content,
        const std::vector<ToolCall>& tool_calls) const;

    /**
     * @brief Chat format identifier (e.g. "chatml").
     * @return Format string, or empty for GGUF-embedded template.
     * @version 1.8.2
     */
    virtual std::string chat_format() const = 0;

protected:
    /* ── Shared parsing primitives ───────────────────────── */

    /**
     * @brief Parse <tool_call>JSON</tool_call> tagged blocks.
     * @version 1.8.2
     */
    std::vector<ToolCall> parse_tagged_tool_calls(
        const std::string& content) const;

    /**
     * @brief Parse bare JSON lines containing "name" key.
     * @version 1.8.2
     */
    std::vector<ToolCall> parse_bare_json_tool_calls(
        const std::string& content) const;

    /**
     * @brief Extract <think>...</think> content.
     * @version 1.8.2
     */
    std::string extract_thinking(const std::string& content) const;

    /**
     * @brief Strip all <think>...</think> blocks from content.
     * @version 1.8.2
     */
    std::string strip_think_blocks(const std::string& content) const;

    /**
     * @brief Attempt JSON recovery on malformed tool call string.
     * @version 1.8.2
     */
    std::optional<ToolCall> try_recover_json(
        const std::string& json_str) const;

    /**
     * @brief Parse a single JSON tool call string.
     * @param json_str JSON from tagged block.
     * @return Parsed ToolCall or nullopt.
     * @version 1.8.2
     */
    std::optional<ToolCall> parse_single_tool_call(
        const std::string& json_str) const;

    std::string tier_name_;          ///< Identity tier name
    std::string identity_prompt_;    ///< Assembled identity prompt
    mutable std::unordered_set<std::string> tool_prefixes_; ///< Known tool prefixes

public:
    /* ── Vision / multimodal (v1.9.11) ────────────────────── */

    /**
     * @brief Format system prompt with optional vision context.
     * @param base_system Base system prompt text.
     * @param has_vision Whether the model has vision capability.
     * @return Formatted system prompt.
     * @version 1.9.11
     *
     * Default implementation returns base_system unchanged.
     * Vision-capable adapters override to append vision instructions.
     */
    virtual std::string format_system_with_vision(
        const std::string& base_system,
        bool has_vision) const;

    /**
     * @brief Convert multimodal content parts to adapter-specific format.
     * @param parts Content parts from a message.
     * @return JSON string in the format expected by the model's chat template.
     * @version 1.9.11
     *
     * Default: OpenAI-format content array. Adapters override if the
     * model expects a different image reference format.
     */
    virtual std::string format_content_parts(
        const std::vector<ContentPart>& parts) const;
};

} // namespace entropic
