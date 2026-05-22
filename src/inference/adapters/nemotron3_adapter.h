// SPDX-License-Identifier: Apache-2.0
/**
 * @file nemotron3_adapter.h
 * @brief Nemotron 3 chat adapter (v2.1.9, gh#47).
 *
 * @par Architecture-verification gate (proposal §gh#47)
 * - **Hybrid Mamba-Transformer** (Mamba-2 + MLP + 4 attention layers),
 *   compressed from NVIDIA-Nemotron-Nano-9B-v2 via Nemotron Elastic.
 * - **GGUF arch tag:** `nemotron_h` (variant `nemotron_h_moe`).
 * - **llama.cpp status:** fully integrated. `LLM_ARCH_NEMOTRON_H` is
 *   in the arch enumeration; `llm_build_nemotron_h` extends
 *   `llm_build_mamba_base` — state handling is shared with the stable
 *   Mamba path, not experimental.
 * - **Chat template:** thinking-enabled by default; `<think>` and
 *   `</think>` are separate special tokens. With llama.cpp CLI use
 *   `--special` to surface them; programmatic generation receives the
 *   tokens already detokenised, so the adapter's base-class
 *   `strip_think_blocks` / `extract_thinking` handle them naturally.
 * - **Tool-call format:** the vLLM docs advertise the `qwen3_coder` XML
 *   parser, but empirical capture (gh#70, v2.3.8) showed the bundled
 *   `nemotron_h` GGUFs actually emit a **DSML invoke** format at every
 *   precision (Q4_K_XL / Q8_0 / BF16):
 *   @code
 *   <｜DSML｜function_calls>
 *   <｜DSML｜invoke name="tool.name">
 *   <｜DSML｜parameter name="key" string="value"/>
 *   </｜DSML｜invoke>
 *   </｜DSML｜function_calls>
 *   @endcode
 *   (fullwidth-pipe `｜` = U+FF5C; self-closing typed parameter tags).
 *   The adapter parses DSML first, then falls back to the qwen XML and
 *   tagged-JSON paths for rigged-prompt / mixed-format consumers.
 * - **Reasoning trace:** yes — handled by base-class think-block
 *   primitives; no Nemotron-specific override needed.
 *
 * Gate outcome: **PASSES.** Nemotron3Adapter proceeds.
 *
 * Internal to inference .so.
 *
 * @version 2.3.8
 */

#pragma once

#include <entropic/inference/adapters/adapter_base.h>

#include <unordered_map>

namespace entropic {

/**
 * @brief Nemotron 3 chat adapter (hybrid Mamba-Transformer family).
 *
 * Tool-call parsing mirrors the qwen3_coder XML format. Reasoning
 * traces are emitted as `<think>...</think>` blocks and handled by
 * the shared base-class primitives.
 *
 * @version 2.1.9
 */
class Nemotron3Adapter : public ChatAdapter {
public:
    using ChatAdapter::ChatAdapter;

    /**
     * @brief Chat format: GGUF-embedded template (Nemotron-specific).
     * @return Empty string — llama.cpp drives template application
     *         from the GGUF's stored template.
     * @utility
     * @version 2.1.9
     */
    std::string chat_format() const override { return ""; }

    /**
     * @brief Parse DSML invoke calls; fall back to qwen XML, then tagged JSON.
     * @param content Raw model output.
     * @return ParseResult.
     * @version 2.3.8
     */
    ParseResult parse_tool_calls(const std::string& content) const override;

    /**
     * @brief Wrap tool result in `<tool_response>` tags.
     * @param tool_call Executed tool call.
     * @param result Execution result text.
     * @return User-role message containing the wrapped result.
     * @version 2.1.9
     */
    Message format_tool_result(
        const ToolCall& tool_call,
        const std::string& result) const override;

protected:
    /**
     * @brief Format tools as a `<tools>` JSON array, then teach DSML invoke.
     * @param tool_jsons Tool definition JSON strings.
     * @return Section to inject into the system prompt.
     * @version 2.3.8
     */
    std::string format_tools(
        const std::vector<std::string>& tool_jsons) const override;

private:
    /**
     * @brief Parse `<｜DSML｜invoke name="X">...</｜DSML｜invoke>` blocks.
     *
     * Primary parser for Nemotron 3 (gh#70). Matches each invoke block
     * (whether or not wrapped in `<｜DSML｜function_calls>`) and extracts
     * its self-closing typed parameter tags.
     *
     * @param content Model output.
     * @return Vector of parsed tool calls.
     * @internal
     * @version 2.3.8
     */
    std::vector<ToolCall> parse_dsml_function_calls(
        const std::string& content) const;

    /**
     * @brief Extract `<｜DSML｜parameter name="K" string="V"/>` pairs.
     *
     * Accepts the `string` / `int` / `bool` / `value` typed attribute
     * keywords; the captured value is stored verbatim (no JSON quoting),
     * matching the qwen XML parameter convention.
     *
     * @param invoke_body Invoke block body text.
     * @return Map of parameter key -> value.
     * @internal
     * @version 2.3.8
     */
    std::unordered_map<std::string, std::string> extract_dsml_parameters(
        const std::string& invoke_body) const;

    /**
     * @brief Parse `<function=name><parameter=key>value</parameter></function>` blocks.
     * @param content Model output.
     * @return Vector of parsed tool calls.
     * @internal
     * @version 2.1.9
     */
    std::vector<ToolCall> parse_xml_function_calls(
        const std::string& content) const;

    /**
     * @brief Extract `<parameter=...>...</parameter>` pairs from a function body.
     * @param func_body Function body text.
     * @return Map of parameter key -> value.
     * @internal
     * @version 2.1.9
     */
    std::unordered_map<std::string, std::string> extract_xml_parameters(
        const std::string& func_body) const;

    /**
     * @brief Strip tool calls and think blocks from content.
     * @param content Raw model output.
     * @return Cleaned content.
     * @internal
     * @version 2.1.9
     */
    std::string clean_content(const std::string& content) const;
};

} // namespace entropic
