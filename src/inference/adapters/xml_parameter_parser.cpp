// SPDX-License-Identifier: Apache-2.0
/**
 * @file xml_parameter_parser.cpp
 * @brief Shared XML `<parameter=...>value</parameter>` extractor.
 * @version 2.4.1 (gh#79)
 */

#include "xml_parameter_parser.h"

#include <regex>
#include <string>
#include <utility>

namespace entropic::inference::adapters {

namespace {

/**
 * @brief Trim leading/trailing whitespace (space, tab, CR, LF).
 * @utility
 * @version 2.4.1
 */
inline std::string trim_ws(const std::string& s) {
    auto first = s.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) { return {}; }
    auto last = s.find_last_not_of(" \t\n\r");
    return s.substr(first, last - first + 1);
}

/**
 * @brief Compiled-once regex matching `<parameter=NAME>` opening tags.
 * @utility
 * @version 2.4.1
 */
const std::regex& open_tag_pattern() {
    static const std::regex pat(R"(<parameter=([^>]+)>)");
    return pat;
}

/**
 * @brief Truncate body at the first nested `<function=` tag.
 *
 * Malformed multi-call output handling preserved from the
 * per-adapter implementations. Emits a warn on truncation.
 *
 * @utility
 * @version 2.4.1
 */
inline std::string truncate_at_nested_function(
    const std::string& body,
    const std::shared_ptr<spdlog::logger>& logger) {
    auto nested = body.find("<function=");
    if (nested == std::string::npos) { return body; }
    if (logger) {
        logger->warn("Truncating function body at nested <function= tag");
    }
    return body.substr(0, nested);
}

/**
 * @brief Locate the close tag for a `<parameter=NAME>` opening.
 *
 * gh#79: accept whichever of `</parameter>` or `</NAME>` appears
 * first after `value_offset`. Returns (npos, 0) when neither is
 * present.
 *
 * @return Pair of (end_pos, close_tag_length).
 * @utility
 * @version 2.4.1
 */
inline std::pair<size_t, size_t> find_close_tag(
    const std::string& body, size_t value_offset, const std::string& key) {
    const std::string close_paren = "</parameter>";
    const std::string close_named = "</" + key + ">";
    const auto p_pos = body.find(close_paren, value_offset);
    const auto n_pos = body.find(close_named, value_offset);
    if (p_pos == std::string::npos && n_pos == std::string::npos) {
        return {std::string::npos, 0};
    }
    if (p_pos < n_pos) { return {p_pos, close_paren.size()}; }
    return {n_pos, close_named.size()};
}

/**
 * @brief Insert a trimmed value into the arguments map (skipping empty).
 * @utility
 * @version 2.4.1
 */
inline void emit_arg(
    std::unordered_map<std::string, std::string>& args,
    const std::string& key,
    const std::string& raw_value,
    const std::shared_ptr<spdlog::logger>& logger) {
    auto value = trim_ws(raw_value);
    if (value.empty()) {
        if (logger) {
            logger->warn("Skipping empty XML parameter value: key='{}'", key);
        }
        return;
    }
    args[key] = std::move(value);
}

}  // namespace

/**
 * @brief Extract `<parameter=NAME>value</parameter>` pairs from a function body.
 *
 * gh#79 fix: tolerates `</NAME>` close tags in addition to the
 * literal `</parameter>`. See the helper definitions above and the
 * header for full contract.
 *
 * @utility
 * @version 2.4.1
 */
std::unordered_map<std::string, std::string> parse_xml_parameters(
    const std::string& func_body,
    const std::shared_ptr<spdlog::logger>& logger) {

    const std::string body = truncate_at_nested_function(func_body, logger);
    std::unordered_map<std::string, std::string> arguments;

    auto search_start = body.cbegin();
    std::smatch open_match;
    while (std::regex_search(search_start, body.cend(),
                             open_match, open_tag_pattern())) {
        const std::string key = trim_ws(open_match[1].str());
        const auto value_begin = open_match[0].second;
        const auto value_offset =
            static_cast<size_t>(std::distance(body.cbegin(), value_begin));

        if (key.empty()) {
            if (logger) { logger->warn("Skipping <parameter=> with empty key"); }
            search_start = value_begin;
            continue;
        }

        const auto [end_pos, close_len] = find_close_tag(body, value_offset, key);
        if (end_pos == std::string::npos) {
            if (logger) {
                logger->warn(
                    "Skipping <parameter={}> with no closing tag", key);
            }
            search_start = value_begin;
            continue;
        }

        emit_arg(arguments, key,
                 body.substr(value_offset, end_pos - value_offset), logger);

        search_start = body.cbegin()
                     + static_cast<std::ptrdiff_t>(end_pos + close_len);
    }

    return arguments;
}

}  // namespace entropic::inference::adapters
