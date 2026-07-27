// SPDX-License-Identifier: Apache-2.0
/**
 * @file final_text.h
 * @brief Operator-visible final-text extraction for the external bridge.
 *
 * Private facade header. Extracted from external_bridge.cpp in v2.10.2 so
 * the selection rule is directly unit-testable — gh#130 was a one-line
 * scanning bug in a `static` helper with no test reachable to it.
 *
 * @version 2.10.2
 */

#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace facade_text {

/**
 * @brief Extract the answer text from a serialized conversation.
 *
 * Scans backwards for the most recent assistant message that actually has
 * content, **skipping empty ones**.
 *
 * @par Why empties must be skipped (gh#130)
 * A turn that ends without `entropic.complete` — e.g. anti-spiral rejects
 * the lead's tool call and the next generation comes back
 * `finish=stop, 0 tool calls, 0 chars` — leaves a trailing **empty**
 * assistant message. Returning that empty content and stopping made the
 * bridge answer `"(no response)"` while the real answer sat one or two
 * messages earlier.
 *
 * The worst case was a completed sub-tier delegation: `fold_delegation_summary`
 * (gh#119, v2.9.17) already folds a child's summary into the lead's empty
 * assistant turn precisely so this function can find it — but a *later*
 * terminal empty assistant turn shadowed it, and the operator lost an answer
 * the engine had correctly produced.
 *
 * @par Why earlier user-role messages are NOT a fallback
 * Tool and delegation results are injected as `role: "user"`
 * (`tool_executor.cpp`), and `serialize_messages` emits only
 * `{role, content}` — so at this layer a delegation summary is
 * indistinguishable from the operator's own prompt. Falling back to the last
 * user message would echo the operator's question back as the answer, which
 * is worse than admitting there is none. Delegation summaries reach us via
 * the assistant-turn fold above, not by sniffing user messages.
 *
 * @param result_json Serialized conversation JSON array.
 * @return Most recent non-empty assistant content, or empty if there is none.
 * @utility
 * @version 2.10.2
 */
inline std::string extract_final_text(const char* result_json) {
    if (result_json == nullptr) {
        return {};
    }
    auto arr = nlohmann::json::parse(result_json, nullptr, false);
    if (!arr.is_array()) {
        return {};
    }
    for (auto it = arr.rbegin(); it != arr.rend(); ++it) {
        if (!it->is_object() || it->value("role", "") != "assistant") {
            continue;
        }
        auto content = it->value("content", "");
        if (!content.empty()) {
            return content;
        }
    }
    return {};
}

/**
 * @brief Explain why no answer text could be extracted (gh#130).
 *
 * The bare `"(no response)"` the bridge used to return could not distinguish
 * an engine failure from a model that simply stalled, which the consumer
 * reported as its own diagnostic dead end. The returned strings keep
 * `"(no response"` as their leading substring so prefix/substring matching on
 * the old sentinel still fires; exact-equality matching does not.
 *
 * Deliberately reports only what the conversation itself proves. Iteration
 * counts and the specific precondition that fired (anti-spiral, budget) are
 * not present in this JSON, so they are not invented here.
 *
 * @param result_json Serialized conversation JSON array.
 * @return Human-readable diagnostic for the empty-answer case.
 * @utility
 * @version 2.10.2
 */
inline std::string no_response_reason(const char* result_json) {
    auto arr = (result_json != nullptr)
                   ? nlohmann::json::parse(result_json, nullptr, false)
                   : nlohmann::json();
    if (!arr.is_array()) {
        return "(no response: the engine returned no readable conversation)";
    }

    bool saw_assistant = false;
    for (const auto& msg : arr) {
        if (msg.is_object() && msg.value("role", "") == "assistant") {
            saw_assistant = true;
            break;
        }
    }
    if (!saw_assistant) {
        return "(no response: the turn produced no assistant message at all)";
    }
    return "(no response: the turn ended with every assistant message empty — "
           "the tier most likely stopped without calling entropic.complete)";
}

/**
 * @brief Final answer text, or a diagnostic when there is none.
 * @param result_json Serialized conversation JSON array.
 * @return Answer text, else the reason there is none.
 * @utility
 * @version 2.10.2
 */
inline std::string final_text_or_reason(const char* result_json) {
    auto text = extract_final_text(result_json);
    return text.empty() ? no_response_reason(result_json) : text;
}

}  // namespace facade_text
