// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file utf8_sanitize.h
 * @brief UTF-8 validation + replacement for inbound tool-result strings.
 *
 * MCP server subprocesses can produce JSON-RPC tool results whose
 * string content (file bytes, command output, scraped HTML) carries
 * invalid UTF-8 sequences — legacy codepage source comments, mojibake,
 * incomplete trailing bytes from truncation. nlohmann::json::dump()
 * throws json::type_error 316 on such bytes, which aborts the
 * agent loop downstream of any place that re-serializes the message.
 *
 * The remedy lives at the inbound boundary: scrub bytes the moment
 * they arrive from the subprocess, before the engine handles them as
 * "valid string content". sanitize_utf8() walks the input as a
 * byte sequence, validates each codepoint per RFC 3629, and writes
 * U+FFFD (REPLACEMENT CHARACTER, 0xEF 0xBF 0xBD) in place of any
 * malformed sequence. ASCII and well-formed multi-byte input pass
 * through verbatim with no allocations beyond the output string.
 *
 * @version 2.1.0
 */

#pragma once

#include <entropic/entropic_export.h>

#include <string>
#include <string_view>

namespace entropic::mcp {

/**
 * @brief Replace invalid UTF-8 byte sequences with U+FFFD.
 *
 * @param input Raw bytes from a tool-result subprocess. Treated as a
 *              byte sequence, not a code-point sequence.
 * @return A new string equal to ``input`` if already valid UTF-8,
 *         or with each malformed byte sequence replaced by U+FFFD
 *         (the Unicode replacement character).
 *
 * @par Algorithm:
 * Per RFC 3629:
 *   - 0x00..0x7F            → 1-byte ASCII, pass through
 *   - 0xC2..0xDF + 1 cont   → 2-byte sequence
 *   - 0xE0..0xEF + 2 cont   → 3-byte sequence (with sub-range checks
 *                             on the first continuation byte)
 *   - 0xF0..0xF4 + 3 cont   → 4-byte sequence (with sub-range checks)
 *   - anything else         → replace with U+FFFD, advance one byte
 *
 * Continuation bytes must be in 0x80..0xBF; a missing or out-of-range
 * continuation triggers replacement and advances past the leading byte
 * only (the next byte gets a fresh validation pass — Bjoern Hoehrmann's
 * "robust resync" property).
 *
 * @utility
 * @version 2.1.0
 */
ENTROPIC_EXPORT std::string sanitize_utf8(std::string_view input);

} // namespace entropic::mcp
