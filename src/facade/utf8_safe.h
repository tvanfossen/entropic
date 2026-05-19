// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <cstddef>

/**
 * @file utf8_safe.h
 * @brief UTF-8-boundary-aware string truncation helper for the facade.
 *
 * Byte-indexed `std::string::substr` slices through multi-byte UTF-8
 * codepoints, producing invalid bytes that `nlohmann::json::dump()` rejects
 * with `type_error.316`. This header exposes a small helper that rounds the
 * cut DOWN to the previous codepoint boundary by walking back over UTF-8
 * continuation bytes (`0x80..0xBF`). Defined in a header so it can be unit
 * tested from `tests/unit/api/` without exporting a public C symbol. (gh#56)
 */

namespace entropic::facade {

/**
 * @brief Truncate a UTF-8 string at-or-before a byte cap on a codepoint boundary.
 * @utility
 *
 * @param s         Input string (treated as UTF-8 bytes).
 * @param max_bytes Maximum byte length of the returned prefix.
 * @return Prefix of `s` with length <= `max_bytes` ending on a codepoint
 *         boundary. If `s` is already <= `max_bytes`, it is returned as-is.
 *         If `s` is valid UTF-8 the returned prefix is also valid UTF-8.
 * @version 2.2.3
 */
inline std::string utf8_safe_substr(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) { return s; }
    std::size_t cut = max_bytes;
    while (cut > 0
           && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut);
}

} // namespace entropic::facade
