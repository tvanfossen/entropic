// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file utf8_sanitize.cpp
 * @brief Implementation of sanitize_utf8().
 * @version 2.1.0
 */

#include <entropic/mcp/utf8_sanitize.h>

#include <cstdint>

namespace entropic::mcp {

namespace {

constexpr char kReplacement[] = "\xEF\xBF\xBD";  // U+FFFD
constexpr size_t kReplacementLen = 3;

/**
 * @brief Test whether b is a UTF-8 continuation byte (0x80..0xBF).
 * @utility
 * @version 2.1.0
 */
inline bool is_cont(uint8_t b) { return (b & 0xC0) == 0x80; }

/**
 * @brief Required follow-bytes for a leading byte; -1 if not a leader.
 *
 * 0x80..0xBF (lone continuation) and 0xC0..0xC1 (overlong starts) and
 * 0xF5..0xFF (out of range) all return -1. Single-return form to keep
 * knots ≤ 3 returns/function happy.
 *
 * @utility
 * @version 2.1.0
 */
inline int follow_count(uint8_t b) {
    int n = -1;
    if (b < 0x80) { n = 0; }
    else if (b >= 0xC2 && b < 0xE0) { n = 1; }
    else if (b >= 0xE0 && b < 0xF0) { n = 2; }
    else if (b >= 0xF0 && b < 0xF5) { n = 3; }
    return n;
}

/**
 * @brief Sub-range check on the first continuation byte for E0/ED/F0/F4.
 *
 * Disallows overlong encodings and surrogate code-points per RFC 3629:
 *   - E0:        0xA0..0xBF
 *   - E1..EC,EE..EF: 0x80..0xBF
 *   - ED:        0x80..0x9F  (excludes UTF-16 surrogates)
 *   - F0:        0x90..0xBF
 *   - F1..F3:    0x80..0xBF
 *   - F4:        0x80..0x8F  (caps codepoint at U+10FFFF)
 * @utility
 * @version 2.1.0
 */
inline bool first_cont_in_range(uint8_t lead, uint8_t c1) {
    bool ok = is_cont(c1);  // default upper-bound 0xBF, lower-bound 0x80
    if (lead == 0xE0) { ok = ok && c1 >= 0xA0; }
    else if (lead == 0xED) { ok = ok && c1 <= 0x9F; }
    else if (lead == 0xF0) { ok = ok && c1 >= 0x90; }
    else if (lead == 0xF4) { ok = ok && c1 <= 0x8F; }
    return ok;
}

/**
 * @brief Validate a single sequence at p; return its length if valid, else 0.
 *
 * Caller guarantees ``p`` is in [start, end). Returns 1 for ASCII,
 * 2/3/4 for valid multi-byte. Returns 0 on any malformedness; caller
 * advances by one byte and emits a replacement for that byte.
 * @utility
 * @version 2.1.0
 */
size_t valid_seq_len(const uint8_t* p, const uint8_t* end) {
    int n = follow_count(*p);
    bool ok = (n >= 0)
           && (n == 0
               || (p + n < end && first_cont_in_range(*p, p[1])));
    for (int i = 2; ok && i <= n; ++i) {
        ok = is_cont(p[i]);
    }
    return ok ? static_cast<size_t>(n + 1) : 0;
}

} // namespace

/**
 * @brief Replace invalid UTF-8 byte sequences with U+FFFD.
 * @param input Raw bytes (potentially malformed).
 * @return Sanitized string; equal to input when input is already valid.
 * @utility
 * @version 2.1.0
 */
std::string sanitize_utf8(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    const auto* p = reinterpret_cast<const uint8_t*>(input.data());
    const auto* end = p + input.size();
    while (p < end) {
        size_t len = valid_seq_len(p, end);
        if (len > 0) {
            out.append(reinterpret_cast<const char*>(p), len);
            p += len;
        } else {
            out.append(kReplacement, kReplacementLen);
            ++p;
        }
    }
    return out;
}

} // namespace entropic::mcp
