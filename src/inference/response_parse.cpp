// SPDX-License-Identifier: Apache-2.0
/**
 * @file response_parse.cpp
 * @brief Template-first / adapter-second parse rule (gh#108).
 * @version 2.10.3
 */

#include "response_parse.h"

#include "llama_cpp_backend.h"

#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <utility>

static auto logger = entropic::log::get("inference.parse");

namespace entropic {

/**
 * @brief Required parameter names declared by a tool's inputSchema.
 * @param tools_json Staged MCP tool defs.
 * @param name Tool name to look up.
 * @param[out] found Set false when the tool is not in the staged set.
 * @return Required parameter names (empty when none/unknown).
 * @utility
 * @version 2.10.3
 */
static std::vector<std::string> required_params(const std::string& tools_json,
                                                const std::string& name,
                                                bool& found) {
    found = false;
    std::vector<std::string> out;
    auto tools = nlohmann::json::parse(tools_json, nullptr, false);
    if (!tools.is_array()) { return out; }
    for (const auto& t : tools) {
        if (!t.is_object() || t.value("name", std::string{}) != name) {
            continue;
        }
        found = true;
        auto req = t.value("inputSchema", nlohmann::json::object())
                    .value("required", nlohmann::json::array());
        for (const auto& r : req) {
            if (r.is_string()) { out.push_back(r.get<std::string>()); }
        }
        break;
    }
    return out;
}

/**
 * @brief Verify every required key is present, warning on the first absent one.
 *
 * Split out of one_call_satisfies_schema to stay under the 3-return gate.
 *
 * @param args Parsed argument object.
 * @param required Declared required parameter names.
 * @param call_name Tool name, for the diagnostic.
 * @return false on the first missing key.
 * @utility
 * @version 2.10.3
 */
static bool has_all_required(const nlohmann::json& args,
                             const std::vector<std::string>& required,
                             const std::string& call_name) {
    for (const auto& key : required) {
        if (!args.contains(key)) {
            logger->warn(
                "Template parse of '{}' is missing required parameter '{}' — "
                "common_chat's PEG autoparser extracts only the first "
                "<parameter=>; falling back to the adapter parser.",
                call_name, key);
            return false;
        }
    }
    return true;
}

/**
 * @brief Check one call's declared required parameters are all present.
 * @param call Parsed tool call.
 * @param tools_json Staged MCP tool defs.
 * @return false only when a declared required parameter is provably absent.
 * @utility
 * @version 2.10.3
 */
static bool one_call_satisfies_schema(const ToolCall& call,
                                      const std::string& tools_json) {
    bool known = false;
    auto required = required_params(tools_json, call.name, known);
    // An unknown tool is not evidence of a truncated parse.
    if (!known || required.empty()) { return true; }

    auto args = nlohmann::json::parse(call.arguments_json, nullptr, false);
    return args.is_object() && has_all_required(args, required, call.name);
}

/**
 * @brief Check that every call's declared required parameters are present.
 * @param calls Parsed tool calls.
 * @param tools_json Staged MCP tool defs.
 * @return true when nothing is provably missing.
 * @internal
 * @version 2.10.3
 */
bool calls_satisfy_schema(const std::vector<ToolCall>& calls,
                          const std::string& tools_json) {
    if (calls.empty() || tools_json.empty()) { return true; }

    for (const auto& call : calls) {
        if (!one_call_satisfies_schema(call, tools_json)) { return false; }
    }
    return true;
}

/**
 * @brief Tool calls from the template parser, when trustworthy.
 * @param llama Backend.
 * @param raw Raw output.
 * @param[out] out Result to populate on success.
 * @return true when the template produced a usable parse.
 * @utility
 * @version 2.10.3
 */
static bool try_template_parse(LlamaCppBackend* llama, const std::string& raw,
                               ParsedModelResponse& out) {
    if (llama == nullptr || !llama->common_chat_parse_reliable()) {
        return false;
    }
    auto parsed = llama->parse_response(raw);
    apply_action_envelope_recovery(parsed.tool_calls, raw);  // gh#88
    if (!calls_satisfy_schema(parsed.tool_calls, llama->active_tools_json())) {
        return false;
    }
    out.content = std::move(parsed.content);
    out.tool_calls = std::move(parsed.tool_calls);
    out.used_template = true;
    return true;
}

/**
 * @brief Parse a raw emission: template first, adapter second.
 * @param llama Backend, or nullptr when not llama.cpp-backed.
 * @param adapter Resolved chat adapter, or nullptr.
 * @param raw Raw model output.
 * @return Cleaned content plus tool calls.
 * @internal
 * @version 2.10.3
 */
ParsedModelResponse parse_model_response(LlamaCppBackend* llama,
                                         ChatAdapter* adapter,
                                         const std::string& raw) {
    ParsedModelResponse out;

    if (!try_template_parse(llama, raw, out)) {
        if (adapter != nullptr) {
            auto parsed = adapter->parse_tool_calls(raw);
            out.content = std::move(parsed.cleaned_content);
            out.tool_calls = std::move(parsed.tool_calls);
        } else {
            out.content = raw;
        }
    }

    // Content cleanup ALWAYS runs the adapter's strip, on either branch.
    // Idempotent: a no-op when the template already removed reasoning. This is
    // what closes the gemma4 hole — before v2.10.3 the template branch was the
    // only one that stripped channels, and it is unreachable without a tooled
    // render.
    if (adapter != nullptr) {
        out.content = adapter->strip_think_blocks(out.content);
    }
    return out;
}

} // namespace entropic
