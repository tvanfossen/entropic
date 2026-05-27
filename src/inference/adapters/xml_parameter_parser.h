// SPDX-License-Identifier: Apache-2.0
/**
 * @file xml_parameter_parser.h
 * @brief Shared `<parameter=NAME>value</parameter>` extractor for XML-format
 *        chat adapters (Qwen 3.5 / Qwen 3.6 / Nemotron 3).
 *
 * Pre-v2.4.1 each adapter carried a byte-identical inline regex parser. They
 * also shared a latent bug (gh#79): when a model closes a parameter with
 * `</NAME>` echoing the opening name instead of the literal `</parameter>`,
 * the non-greedy regex scanned past the wrong close tag to the *next*
 * `</parameter>`, bleeding the following parameter's content into the first
 * parameter's value.
 *
 * This helper centralizes the parse:
 *   - Tolerates `</parameter>` OR `</NAME>` close tags (whichever comes first
 *     after the opening, where NAME matches the opening's name verbatim).
 *   - Truncates the body at a nested `<function=` tag (malformed multi-call
 *     output handling, preserved from the per-adapter implementations).
 *   - Trims whitespace on key and value; skips empty after trim.
 *
 * @version 2.4.1 (gh#79)
 */

#pragma once

#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace entropic::inference::adapters {

/**
 * @brief Extract `<parameter=NAME>value</parameter>` pairs from a function body.
 *
 * Returns a map keyed by parameter name. Empty keys / values (after trim) are
 * skipped with a warning. The `</NAME>` close-tag tolerance is the gh#79 fix.
 *
 * @param func_body Function body text (between `<function=name>` and the
 *                  matching `</function>`).
 * @param logger    Adapter-owned logger for warn messages. May be null — the
 *                  parser is silent in that case.
 * @return Map of trimmed parameter key to trimmed parameter value.
 * @utility
 * @version 2.4.1
 */
std::unordered_map<std::string, std::string> parse_xml_parameters(
    const std::string& func_body,
    const std::shared_ptr<spdlog::logger>& logger);

}  // namespace entropic::inference::adapters
