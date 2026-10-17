// SPDX-License-Identifier: Apache-2.0
/**
 * @file final_text.h
 * @brief Operator-visible final-text extraction for the external bridge.
 *
 * Private facade header. Extracted from external_bridge.cpp in v2.10.2 so
 * the selection rule is directly unit-testable — gh#130 was a one-line
 * scanning bug in a `static` helper with no test reachable to it.
 *
 * @version 2.14.5
 */

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_set>

namespace facade_text {

/// @brief Registered tool names (`<server>.<tool>`), for gh#212's bare-name rule.
using ToolNames = std::unordered_set<std::string>;

/**
 * @brief A string member of a message object, or "" when absent or not a string.
 * @param msg Message object.
 * @param key Member name.
 * @return The string value, else empty.
 * @utility
 * @version 2.14.5
 */
inline std::string string_field(const nlohmann::json& msg, const char* key) {
    auto it = msg.find(key);
    return (it != msg.end() && it->is_string()) ? it->get<std::string>()
                                                : std::string{};
}

/**
 * @brief The `name` of every tool in a ServerManager::list_tools() array.
 * @param list_tools_json JSON array of tool definitions.
 * @return The names; empty when the text is not an array.
 * @utility
 * @version 2.14.5
 */
inline ToolNames tool_names_of(const std::string& list_tools_json) {
    ToolNames names;
    auto arr = nlohmann::json::parse(list_tools_json, nullptr, false);
    const size_t count = arr.is_array() ? arr.size() : 0;
    for (size_t i = 0; count > i; ++i) {
        const auto name = arr[i].is_object() ? string_field(arr[i], "name")
                                             : std::string();
        if (!name.empty()) { names.insert(name); }
    }
    return names;
}

/**
 * @brief Drop a surrounding Markdown code fence (```json ... ```), if any.
 * @param text Message content.
 * @return The fenced body, or the trimmed text when there is no fence.
 * @utility
 * @version 2.14.5
 */
inline std::string strip_code_fence(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    std::string body = (first == std::string::npos)
        ? std::string{} : text.substr(first, last - first + 1);
    if (body.rfind("```", 0) == 0) {
        const auto nl = body.find('\n');
        body = (nl == std::string::npos) ? std::string{} : body.substr(nl + 1);
        const auto close = body.rfind("```");
        body = (close == std::string::npos) ? body : body.substr(0, close);
    }
    return body;
}

/**
 * @brief True when an assistant message is a tool call written as TEXT.
 *
 * gh#212 (v2.14.5): after the explicit-completion nudge, a model wrote
 * `{"action": "complete", ...}` — fenced or bare — as its reply instead of
 * calling the tool; in the v2.14.5 gate it wrote just `entropic.inspect`.
 * Neither is an answer, and neither is a call: no tool ran. Such a message
 * is excluded from answer selection and nothing more — the engine never
 * treats it as the call it imitates. The bare-name case matches only a
 * name in `tool_names` exactly, so no ordinary reply can trip it.
 *
 * @param content Assistant message content.
 * @param tool_names Registered tool names; nullptr skips the bare-name rule.
 * @return true for a `{"action":"complete"}` object, or content that is
 *         exactly one registered tool name.
 * @utility
 * @version 2.14.5
 */
inline bool is_completion_imitation(const std::string& content,
                                    const ToolNames* tool_names = nullptr) {
    const auto body = strip_code_fence(content);
    auto obj = nlohmann::json::parse(body, nullptr, false);
    return (obj.is_object() && string_field(obj, "action") == "complete")
        || (tool_names != nullptr && tool_names->count(body) > 0);
}

/**
 * @brief Index just past THIS turn's user prompt; the turn's messages follow.
 *
 * gh#209 (v2.14.5): the scan used to run to the start of the conversation,
 * so a turn with no answer of its own returned the previous turn's. The
 * boundary is the last user message whose content IS the prompt the caller
 * sent. When it cannot be found (nullptr prompt = unbounded, the pre-2.14.5
 * rule), a non-null prompt that is absent yields an EMPTY range: no answer
 * is better than somebody else's.
 *
 * @param arr Serialized conversation array.
 * @param turn_prompt The prompt that started this turn; nullptr = unbounded.
 * @return First index belonging to the turn.
 * @utility
 * @version 2.14.5
 */
inline size_t turn_start(const nlohmann::json& arr, const char* turn_prompt) {
    size_t start = (turn_prompt == nullptr) ? 0 : arr.size();
    for (size_t i = arr.size(); turn_prompt != nullptr && i > 0; --i) {
        const auto& msg = arr[i - 1];
        if (msg.is_object() && string_field(msg, "role") == "user"
            && string_field(msg, "content") == turn_prompt) {
            start = i;
            break;
        }
    }
    return start;
}

/**
 * @brief An assistant message's text when it is an answer, else "".
 * @param msg Message object.
 * @param tool_names Registered tool names, or nullptr.
 * @return Content of a non-empty assistant message that is not a tool
 *         call written as text.
 * @utility
 * @version 2.14.5
 */
inline std::string substantive_assistant_text(const nlohmann::json& msg,
                                              const ToolNames* tool_names) {
    std::string text;
    if (msg.is_object() && string_field(msg, "role") == "assistant") {
        text = string_field(msg, "content");
    }
    return is_completion_imitation(text, tool_names) ? std::string{} : text;
}

/**
 * @brief Extract the answer text from a serialized conversation.
 *
 * Scans backwards for the most recent assistant message that actually has
 * content, **skipping empty ones** — within THIS turn when `turn_prompt` is
 * given (gh#209), and never returning a completion call written as text
 * (gh#212).
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
 * @par Why the scan stops at the turn's prompt (gh#209)
 * Skipping empties without a boundary walked past the start of the turn: a
 * turn that produced nothing returned the PREVIOUS turn's answer, byte for
 * byte, as its own — well-formed, cited, and about a different question.
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
 * @param turn_prompt The prompt that started this turn; nullptr scans the
 *        whole conversation.
 * @param tool_names Registered tool names, or nullptr (gh#212).
 * @return Most recent substantive assistant content in range, or empty.
 * @req REQ-API-012
 * @version 2.14.5
 */
inline std::string extract_final_text(const char* result_json,
                                      const char* turn_prompt = nullptr,
                                      const ToolNames* tool_names = nullptr) {
    auto arr = (result_json != nullptr)
                   ? nlohmann::json::parse(result_json, nullptr, false)
                   : nlohmann::json();
    std::string found;
    if (arr.is_array()) {
        const auto start = turn_start(arr, turn_prompt);
        for (size_t i = arr.size(); i > start && found.empty(); --i) {
            found = substantive_assistant_text(arr[i - 1], tool_names);
        }
    }
    return found;
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
 * @req REQ-API-012
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
 * @param turn_prompt The prompt that started this turn; nullptr = unbounded.
 * @param tool_names Registered tool names, or nullptr (gh#212).
 * @return Answer text, else the reason there is none.
 * @req REQ-API-012
 * @version 2.14.5
 */
inline std::string final_text_or_reason(const char* result_json,
                                        const char* turn_prompt = nullptr,
                                        const ToolNames* tool_names = nullptr) {
    auto text = extract_final_text(result_json, turn_prompt, tool_names);
    return text.empty() ? no_response_reason(result_json) : text;
}

/**
 * @brief A failed turn's reason, plus the turn's unconfirmed text if any.
 *
 * gh#209/#212 (v2.14.5). This is what `entropic_last_error()` carries for
 * ENTROPIC_ERROR_TURN_FAILED. The reason leads, so the text cannot be
 * mistaken for an answer. When the turn wrote something substantive before
 * failing it follows, labelled: in gh#212 the correct answer came two
 * messages before the failure, and dropping it would throw away work the
 * caller can still judge. It travels in the error text because the failed
 * turn is rolled out of the session, and the streaming entry points have no
 * other channel to deliver it.
 *
 * @param reason Why the engine failed the turn.
 * @param turn_json Serialized messages of the failed turn.
 * @param tool_names Registered tool names, or nullptr.
 * @return "<reason>", plus the labelled text when there is any.
 * @req REQ-API-012
 * @version 2.14.5
 */
inline std::string failure_detail(const std::string& reason,
                                  const char* turn_json,
                                  const ToolNames* tool_names) {
    std::string out = reason;
    const auto draft = extract_final_text(turn_json, nullptr, tool_names);
    if (!draft.empty()) {
        out += "\n\nUnconfirmed text from this turn (the turn did not complete, "
               "so treat it as unverified):\n" + draft;
    }
    return out;
}

}  // namespace facade_text
