// SPDX-License-Identifier: Apache-2.0
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
 * @brief The reasoning-block delimiters a model family emits (gh#108).
 *
 * Thinking format is a property of the chat template, not of the transport:
 * Qwen/Nemotron wrap reasoning in `<think>…</think>`, Gemma-4 QAT uses
 * `<|channel>…<channel|>`. Declaring the pair once per adapter keeps the
 * buffered strip (ChatAdapter::strip_think_blocks) and the live stream filter
 * (StreamThinkFilter) from drifting apart — before v2.10.3 they were two
 * independent hardcoded literals and gemma4 matched neither.
 *
 * @version 2.10.3
 */
struct ThinkMarkers {
    std::string open  = "<think>";   ///< Opening delimiter
    std::string close = "</think>";  ///< Closing delimiter
};

/**
 * @brief gh#88: recover tool calls a gemma model parroted as bare-JSON.
 *
 * common_chat (PEG_GEMMA4) parses only the native call-prefix form. When a
 * session primes the model with `{"action":"x",...}` meta-tool result
 * envelopes (see the engine defang), the model echoes that bare-JSON shape
 * and the dedicated grammar yields nothing. This permissive fallback
 * (parity with the retired Gemma4Adapter) maps a `{"action":"<tool>",...}`
 * envelope to the `entropic.<tool>` call (remaining fields as arguments),
 * and also accepts a plain `{"name":...}` object.
 *
 * @param raw Raw assistant output.
 * @return Recovered tool calls (empty when none match).
 * @version 2.7.1
 */
std::vector<ToolCall> recover_action_envelope_calls(const std::string& raw);

/**
 * @brief gh#88: substitute recovered bare-JSON calls when a reliable
 *        (PEG_GEMMA4 / gemma) parse produced none; logs the recovery.
 *
 * No-op unless @p calls is empty AND a recovery is found. The WARN keeps
 * residual/future context-priming visible instead of silently masking it.
 * Used only on the common_chat-reliable path (interface_factory /
 * orchestrator).
 *
 * @param calls In/out parsed calls; replaced by the recovery iff empty + found.
 * @param raw   Raw assistant output to recover from.
 * @version 2.7.1
 */
void apply_action_envelope_recovery(std::vector<ToolCall>& calls,
                                    const std::string& raw);

/**
 * @brief gh#90: coerce numeric scalars back to strings for string-typed
 *        tool parameters.
 *
 * gemma's `<|"|>` string-escape loses type through PEG_GEMMA4 — a
 * numeric-looking string arg (e.g. `grade_level:<|"|>3<|"|>`) arrives as a
 * JSON number, a string-typed schema then rejects it (`3 is not of type
 * 'string'`), and the delegation circuit-breaks. For each call, when the
 * staged tool schema declares a parameter `"type":"string"` and the parsed
 * value is a JSON number, coerce it to its string form (updates both
 * `arguments_json` and the `arguments` map). No-op when @p tools_json is
 * empty/invalid or the parameter is genuinely non-string. Applied on the
 * common_chat-reliable (gemma) parse path.
 *
 * @param calls      In/out parsed tool calls.
 * @param tools_json Staged MCP tool defs (array of {name, inputSchema,...}).
 * @version 2.7.2
 */
void coerce_string_typed_args(std::vector<ToolCall>& calls,
                              const std::string& tools_json);

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


    /* ── Tool call parsing (subclass overrides) ──────────── */

    /**
     * @brief Parse tool calls from model output.
     * @param content Raw model output.
     * @return ParseResult with cleaned content and tool calls.
     * @version 1.8.2
     */
    /**
     * @brief This family's reasoning-block delimiters (gh#108).
     *
     * Default is the `<think>` pair used by Qwen and Nemotron. Override in a
     * family that wraps reasoning differently — Gemma-4 QAT does.
     *
     * @return Marker pair.
     * @utility
     * @version 2.10.3
     */
    virtual ThinkMarkers thinking_markers() const { return {}; }

    /**
     * @brief Strip this family's reasoning blocks from content (gh#108).
     *
     * Uses thinking_markers(), so an adapter declares its markers once and
     * both the buffered strip and the live StreamThinkFilter agree. Matching
     * is plain substring, not regex — `<|channel>` contains a regex
     * metacharacter and would need escaping.
     *
     * An unclosed opening marker (generation hit its token budget
     * mid-reasoning) erases to end of content and WARNs: no answer was ever
     * produced, so returning the reasoning would be worse than nothing.
     *
     * PUBLIC since v2.10.3 — was protected. The shared parse rule
     * (response_parse.h) runs it on both the template and adapter branches,
     * so it is an inference-layer concern now, not a subclass helper.
     * Widening only; non-virtual, so no ABI change for existing consumers.
     *
     * @param content Model output.
     * @return Content with reasoning blocks removed and trimmed.
     * @version 2.10.3
     */
    std::string strip_think_blocks(const std::string& content) const;

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
