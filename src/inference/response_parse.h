// SPDX-License-Identifier: Apache-2.0
/**
 * @file response_parse.h
 * @brief One template-first / adapter-second parse rule for raw model output.
 *
 * @par Why this exists (gh#108, v2.10.3)
 * The same branch was written twice — `orchestrator.cpp` (buffered generate
 * result) and `interface_factory.cpp` (agent-loop tool parse) — each choosing
 * between llama.cpp's `common_chat` parser and the engine's ChatAdapter. The
 * duplication is how a fix reached one path and not the other.
 *
 * It also encoded the wrong relationship. `common_chat_parse_reliable()` is
 * `parse_params_valid_ && format == PEG_GEMMA4`: it exists to say *"this
 * captured format is multi-parameter safe"*, and thinking-block removal got
 * bolted onto it. So reasoning stripping only happened for gemma4, and only
 * when a TOOLED render had captured a parser arena — a toolless generate fell
 * to the adapter branch with no channel handling at all.
 *
 * The rule here separates the two concerns, because they compose differently:
 *
 *  - **Content cleanup composes.** Run the template parse when an arena
 *    exists, then ALWAYS run the adapter's strip over the result. Stripping is
 *    idempotent — if the template already removed reasoning, the second pass
 *    is a no-op — so no decision logic is needed and no path can be missed.
 *
 *  - **Tool-call extraction does not compose.** Two call lists cannot be
 *    merged safely, so it is genuinely either/or. The template wins when its
 *    result is trustworthy; the adapter is the fallback.
 *
 * @par Why the tool-call fallback needs an a-posteriori check
 * `common_chat`'s PEG autoparser extracts only the FIRST `<parameter=>` of a
 * multi-parameter call (gh#87 Phase D) — and it does so *silently*, returning
 * a well-formed ToolCall with arguments missing. A try/catch fallback would
 * never fire. So the template result is validated against the staged tool
 * schema, and a call missing declared `required` parameters routes to the
 * adapter's hand-rolled parser (`xml_parameter_parser`, which also tolerates
 * the gh#79 `</NAME>` close tag). Without this, moving Qwen/Nemotron onto the
 * template path would quietly drop arguments.
 *
 * @version 2.10.3
 */

#pragma once

#include <entropic/inference/adapters/adapter_base.h>
#include <entropic/types/tool_call.h>

#include <string>
#include <vector>

namespace entropic {

class LlamaCppBackend;

/**
 * @brief Content and tool calls extracted from one raw emission.
 * @version 2.10.3
 */
struct ParsedModelResponse {
    std::string content;               ///< Reasoning-stripped content
    std::vector<ToolCall> tool_calls;  ///< Extracted calls
    bool used_template = false;        ///< True when common_chat produced the calls
};

/**
 * @brief Check that every call's declared required parameters are present.
 *
 * The detector for common_chat's silent first-parameter-only extraction. A
 * call naming a tool absent from @p tools_json is left alone — unknown tools
 * are not this function's business.
 *
 * @param calls Parsed tool calls.
 * @param tools_json Staged MCP tool defs (array of {name, inputSchema, ...}).
 * @return true when nothing is provably missing.
 * @version 2.10.3
 */
bool calls_satisfy_schema(const std::vector<ToolCall>& calls,
                          const std::string& tools_json);

/**
 * @brief Parse a raw emission: template first, adapter second.
 *
 * @param llama Backend, or nullptr when not llama.cpp-backed.
 * @param adapter Resolved chat adapter, or nullptr.
 * @param raw Raw model output.
 * @return Cleaned content plus tool calls.
 * @version 2.10.3
 */
ParsedModelResponse parse_model_response(LlamaCppBackend* llama,
                                         ChatAdapter* adapter,
                                         const std::string& raw);

} // namespace entropic
