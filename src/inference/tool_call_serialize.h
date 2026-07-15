// SPDX-License-Identifier: Apache-2.0
/**
 * @file tool_call_serialize.h
 * @brief Shared serialization of parsed tool calls to the C-ABI JSON array form.
 *
 * gh#93: production (`interface_factory.cpp`) and the model-test harness
 * (`tests/model/model_test_context.h`) each carried their own copy of this.
 * The harness copy stored every argument value as a raw JSON string, while
 * production parsed each value into its natural JSON type — a silent
 * harness↔production fidelity gap (the class of divergence that hid gh#88/90/94).
 * This single source of truth (the typed form) closes that gap by construction:
 * both sites now emit exactly what a consumer receives across the `.so` boundary.
 *
 * @version 2.9.16
 */

#pragma once

#include <string>
#include <vector>

#include <entropic/types/tool_call.h>
#include <nlohmann/json.hpp>

namespace entropic {

/**
 * @brief Serialize parsed tool calls to the C-ABI JSON array form.
 *
 * Each argument value is parsed into its natural JSON type when it is valid
 * JSON (number, bool, array, object, null); otherwise it is emitted as a JSON
 * string. This mirrors what consumers receive across the `.so` boundary.
 *
 * gh#118 (v2.9.16): XML-parsed argument strings carry raw model bytes;
 * MTP-split codepoints cause nlohmann::json::dump() to throw type_error.316
 * INSIDE the inference_.parse_tool_calls callback, before the engine's
 * mcp::sanitize_utf8 guard at parse_tool_calls:1386 can run. Dump with
 * error_handler_t::replace to substitute U+FFFD for any invalid byte
 * sequences — consistent with the sanitize_utf8 policy used at all
 * other inbound UTF-8 boundaries.
 *
 * @param calls Tool calls (from common_chat or a legacy adapter).
 * @return JSON array: `[{name, arguments}, ...]`.
 * @utility
 * @version 2.9.16
 */
inline std::string serialize_tool_calls(
    const std::vector<ToolCall>& calls) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& tc : calls) {
        nlohmann::json args;
        for (const auto& [k, v] : tc.arguments) {
            auto parsed_val = nlohmann::json::parse(v, nullptr, false);
            args[k] = parsed_val.is_discarded()
                ? nlohmann::json(v) : parsed_val;
        }
        arr.push_back({{"name", tc.name}, {"arguments", args}});
    }
    return arr.dump(-1, ' ', false,
                   nlohmann::json::error_handler_t::replace);
}

}  // namespace entropic
