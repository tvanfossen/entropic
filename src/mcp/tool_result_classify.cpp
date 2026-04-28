// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file tool_result_classify.cpp
 * @brief Implementation of the tool-result content classifiers (#44).
 * @version 2.1.0
 */

#include <entropic/mcp/tool_result_classify.h>

#include <cctype>

namespace entropic::mcp {

namespace {

/**
 * @brief Case-insensitive ASCII prefix match.
 * @utility
 * @version 2.1.0
 */
inline bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) { return false; }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i]))
                != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Skip leading ASCII whitespace; return the remaining view.
 * @utility
 * @version 2.1.0
 */
inline std::string_view ltrim(std::string_view s) {
    size_t i = 0;
    while (i < s.size()
           && std::isspace(static_cast<unsigned char>(s[i])) != 0) {
        ++i;
    }
    return s.substr(i);
}

} // namespace

/**
 * @brief True if input is empty or contains only ASCII whitespace.
 * @utility
 * @version 2.1.0
 */
bool is_effectively_empty(std::string_view s) {
    for (unsigned char c : s) {
        if (std::isspace(c) == 0) { return false; }
    }
    return true;
}

/**
 * @brief True if input begins with a JSON top-level "error" key.
 * @utility
 * @version 2.1.0
 */
inline bool starts_with_json_error(std::string_view trimmed) {
    if (trimmed.empty() || trimmed.front() != '{') { return false; }
    auto pos = trimmed.find("\"error\"");
    return pos != std::string_view::npos && pos < 32;
}

/**
 * @brief Truncate content in place when it exceeds the byte cap.
 * @utility
 * @version 2.1.1-rc1
 */
void truncate_to_cap(std::string& content, int cap) {
    if (cap <= 0) { return; }
    auto cap_sz = static_cast<size_t>(cap);
    if (content.size() <= cap_sz) { return; }
    size_t lost = content.size() - cap_sz;
    std::string tail = "\n[... truncated, "
        + std::to_string(lost) + " more bytes]";
    size_t keep = (cap_sz > tail.size())
        ? cap_sz - tail.size() : 0;
    content.resize(keep);
    content += tail;
}

/**
 * @brief Heuristic match for an error-shaped tool result string.
 * @utility
 * @version 2.1.0
 */
bool looks_like_tool_error(std::string_view content) {
    auto trimmed = ltrim(content);
    return starts_with_ci(trimmed, "error")
        || starts_with_ci(trimmed, "[error]")
        || starts_with_json_error(trimmed);
}

} // namespace entropic::mcp
