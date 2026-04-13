/**
 * @file stream_think_filter.cpp
 * @brief StreamThinkFilter implementation.
 * @version 2.0.2
 */

#include <entropic/core/stream_think_filter.h>

namespace entropic {

/**
 * @brief Construct with consumer callback.
 * @param cb Consumer callback.
 * @param ud Consumer user_data.
 * @internal
 * @version 2.0.1
 */
StreamThinkFilter::StreamThinkFilter(TokenCallback cb, void* ud)
    : cb_(cb), ud_(ud) {}

/**
 * @brief Set raw (unfiltered) callback.
 * @param cb Raw callback.
 * @param ud Raw user_data.
 * @internal
 * @version 2.0.1
 */
void StreamThinkFilter::set_raw_callback(TokenCallback cb, void* ud) {
    raw_cb_ = cb;
    raw_ud_ = ud;
}

/**
 * @brief Check if accumulated buffer matches a think tag.
 * @param buf Buffer to check.
 * @param is_open Output: true if <think>, false if </think>.
 * @return true if a complete tag was matched.
 * @utility
 * @version 2.0.1
 */
static bool match_tag(const std::string& buf, bool& is_open) {
    if (buf == "<think>") { is_open = true; return true; }
    if (buf == "</think>") { is_open = false; return true; }
    return false;
}

/**
 * @brief Count expected bytes in a UTF-8 sequence from lead byte.
 * @param byte Lead byte.
 * @return Expected total bytes (1-4), or 0 if continuation byte.
 * @utility
 * @version 2.0.2
 */
static int utf8_char_len(unsigned char byte) {
    // Lookup: ASCII | continuation | 2-byte | 3-byte | 4-byte lead.
    if (byte < 0x80) { return 1; }
    int result = 0;  // continuation byte default
    if (byte >= 0xF0) { result = 4; }
    else if (byte >= 0xE0) { result = 3; }
    else if (byte >= 0xC0) { result = 2; }
    return result;
}

/**
 * @brief Emit bytes to consumer, buffering incomplete UTF-8 codepoints.
 *
 * Accumulates bytes and only forwards to cb_ when the current
 * codepoint sequence is complete. This guarantees consumers receive
 * valid UTF-8 at every callback invocation.
 *
 * @param data Byte data to emit.
 * @param len Byte length.
 * @internal
 * @version 2.0.2
 */
void StreamThinkFilter::emit_utf8_safe(const char* data, size_t len) {
    utf8_buf_.append(data, len);

    // Find the last complete codepoint boundary
    size_t safe = utf8_buf_.size();
    if (safe == 0) { return; }

    // Walk backward from end to find any incomplete trailing sequence
    auto* buf = reinterpret_cast<const unsigned char*>(utf8_buf_.data());
    for (size_t i = 1; i <= 4 && i <= safe; ++i) {
        unsigned char c = buf[safe - i];
        int expected = utf8_char_len(c);
        if (expected > 0) {
            // Found a lead byte at position (safe - i)
            size_t available = i; // bytes from lead to end
            if (available < static_cast<size_t>(expected)) {
                // Incomplete sequence — emit up to lead byte
                safe -= i;
            }
            break;
        }
    }

    if (safe > 0) {
        cb_(utf8_buf_.data(), safe, ud_);
    }
    utf8_buf_.erase(0, safe);
}

/**
 * @brief Process a chunk of tokens.
 * @param chunk Token data.
 * @param len Byte length.
 * @internal
 * @version 2.0.2
 */
/**
 * @brief Process one byte through the tag-buffering state machine.
 * @param c Byte to process.
 * @internal
 * @version 2.0.2
 */
void StreamThinkFilter::process_byte(char c) {
    if (tag_buf_.empty() && c != '<') {
        if (!in_think_) { emit_utf8_safe(&c, 1); }
        return;
    }
    tag_buf_ += c;
    bool is_open = false;
    if (match_tag(tag_buf_, is_open)) {
        in_think_ = is_open;
        tag_buf_.clear();
        return;
    }
    if (tag_buf_.size() > 8) {
        if (!in_think_) {
            emit_utf8_safe(tag_buf_.data(), tag_buf_.size());
        }
        tag_buf_.clear();
    }
}

/**
 * @brief Process an incoming token through the think-tag filter.
 * @param chunk Token byte data.
 * @param len Byte length of chunk.
 * @utility
 * @version 2.0.2
 */
void StreamThinkFilter::on_token(const char* chunk, size_t len) {
    // Raw callback always gets everything (unfiltered, no UTF-8 alignment)
    if (raw_cb_) { raw_cb_(chunk, len, raw_ud_); }

    for (size_t i = 0; i < len; ++i) {
        process_byte(chunk[i]);
    }
}

/**
 * @brief Flush buffered partial tag and UTF-8 content.
 * @internal
 * @version 2.0.2
 */
void StreamThinkFilter::flush() {
    if (!tag_buf_.empty() && !in_think_) {
        emit_utf8_safe(tag_buf_.data(), tag_buf_.size());
    }
    tag_buf_.clear();
    // Flush any remaining UTF-8 buffer (may be incomplete at stream end)
    if (!utf8_buf_.empty()) {
        cb_(utf8_buf_.data(), utf8_buf_.size(), ud_);
        utf8_buf_.clear();
    }
}

} // namespace entropic
