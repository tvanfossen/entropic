// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file utf8_sanitize.h
 * @brief UTF-8 validation + replacement at every system boundary where
 *        bytes change ownership.
 *
 * Bytes carrying invalid UTF-8 (legacy-codepage source comments, mojibake,
 * truncated multi-byte runs, model-stream desyncs under XML-tool-call
 * pressure) crash any downstream ``nlohmann::json::dump()`` with
 * ``json::type_error 316``. v2.1.0 introduced the sanitizer at the MCP
 * inbound boundary on the assumption that "sanitize once at inbound, trust
 * downstream" was sufficient. v2.1.1 (issue #3) generalized that to a
 * boundary-of-ownership policy after the demo session showed bytes
 * reaching ``json::dump()`` from paths that bypassed the inbound boundary.
 *
 * @par Policy: sanitize at every external boundary where bytes change
 *      ownership.
 *   - Inbound from MCP servers      → ``src/mcp/tool_executor.cpp``
 *   - Inbound from llama_cpp stream → ``src/core/response_generator.cpp``
 *     (sanitize once at message-finalization, NEVER per-token — a
 *     multi-byte codepoint can split across token boundaries)
 *   - Inbound from audit-log files  → ``src/facade/entropic_audit.cpp``
 *   - Outbound to C-API consumers   → ``src/facade/json_serializers.h``
 *
 * Interior code (engine memory, hook contexts, dedup cache, per-tier
 * routing) trusts the bytes — once a string has crossed an inbound
 * boundary, no further sanitize call is needed. Conversely, do NOT add
 * sanitize calls inside loops or hot interior paths; they belong at the
 * outermost system seam each direction.
 *
 * sanitize_utf8() walks the input as a byte sequence, validates each
 * codepoint per RFC 3629, and writes U+FFFD (REPLACEMENT CHARACTER,
 * 0xEF 0xBF 0xBD) in place of any malformed sequence. ASCII and
 * well-formed multi-byte input pass through verbatim. The namespace is
 * ``entropic::mcp`` for legacy reasons (v2.1.0 introduced it as an
 * MCP-only utility); relocation to a generic namespace would change the
 * exported C++ symbol and is deferred to a future major release.
 *
 * @version 2.1.1
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
