// SPDX-License-Identifier: Apache-2.0
/**
 * @file stream_think_filter.h
 * @brief Streaming filter that strips a model family's reasoning blocks.
 *
 * Wraps a consumer token callback and filters thinking content
 * from the stream. All tokens (including thinking) can optionally
 * be forwarded to a raw log callback. Consumer output is UTF-8
 * codepoint-aligned — incomplete sequences are buffered across chunks.
 *
 * @version 2.0.2
 */

#pragma once

#include <cstddef>
#include <string>

namespace entropic {

/**
 * @brief Token callback type matching the C API signature.
 * @version 2.0.1
 */
using TokenCallback = void (*)(const char*, size_t, void*);

/**
 * @brief Streaming filter that removes <think> blocks from output.
 *
 * Processes tokens character-by-character. Content inside
 * <think>...</think> tags is suppressed from the consumer callback
 * but always forwarded to the raw callback (if set).
 *
 * @version 2.0.1
 */
class StreamThinkFilter {
public:
    /**
     * @brief Construct with consumer callback.
     * @param cb Consumer token callback (receives filtered output).
     * @param ud Consumer user_data.
     * @version 2.0.1
     */
    StreamThinkFilter(TokenCallback cb, void* ud);

    /**
     * @brief Construct with family-specific reasoning delimiters (gh#108).
     *
     * Thinking format is a property of the chat template, not the transport:
     * Qwen/Nemotron use `<think>…</think>`, Gemma-4 QAT uses
     * `<|channel>…<channel|>`. Before v2.10.3 this filter hardcoded the
     * `<think>` pair, so a gemma4 stream passed raw reasoning straight to the
     * consumer — and on the agent-loop streaming path this filter is the ONLY
     * defense, because ResponseGenerator::generate_streaming builds content
     * from its own token accumulator rather than the parsed result.
     *
     * Markers come from ChatAdapter::thinking_markers(); they are passed as
     * plain strings so that core does not depend on inference.
     *
     * @param cb Consumer token callback (receives filtered output).
     * @param ud Consumer user_data.
     * @param open_marker Reasoning-block opening delimiter.
     * @param close_marker Reasoning-block closing delimiter.
     * @version 2.10.3
     */
    StreamThinkFilter(TokenCallback cb, void* ud,
                      std::string open_marker, std::string close_marker);

    /**
     * @brief Set optional raw callback (receives ALL tokens unfiltered).
     * @param cb Raw callback.
     * @param ud Raw user_data.
     * @version 2.0.1
     */
    void set_raw_callback(TokenCallback cb, void* ud);

    /**
     * @brief Process a chunk of tokens.
     * @param chunk Token data.
     * @param len Token byte length.
     * @version 2.0.1
     */
    void on_token(const char* chunk, size_t len);

    /**
     * @brief Flush any buffered partial tag content.
     *
     * Call at end of generation to ensure no content is lost
     * in the tag accumulator.
     *
     * @version 2.0.1
     */
    void flush();

private:
    TokenCallback cb_;      ///< Consumer callback (filtered)
    void* ud_;              ///< Consumer user_data
    TokenCallback raw_cb_ = nullptr; ///< Raw callback (unfiltered)
    void* raw_ud_ = nullptr;         ///< Raw user_data
    /**
     * @brief Match the buffer against this family's delimiters.
     * @param buf Buffer to check.
     * @param is_open Output: true for opening, false for closing.
     * @return true on a complete delimiter match.
     * @utility
     * @version 2.10.3
     */
    bool match_tag(const std::string& buf, bool& is_open) const;

    bool in_think_ = false; ///< Inside a reasoning block

    std::string open_marker_  = "<think>";   ///< Family opening delimiter
    std::string close_marker_ = "</think>";  ///< Family closing delimiter
    /// Longest delimiter — the buffer flushes past this, so it must be
    /// derived, not hardcoded (`<|channel>` is 10 bytes, `</think>` is 8).
    std::size_t max_marker_len_ = 8;
    std::string tag_buf_;   ///< Partial tag accumulator
    std::string utf8_buf_;  ///< Partial UTF-8 codepoint buffer

    /**
     * @brief Emit bytes to consumer, buffering incomplete UTF-8 codepoints.
     * @param data Byte data to emit.
     * @param len Byte length.
     * @version 2.0.2
     */
    void emit_utf8_safe(const char* data, size_t len);

    /**
     * @brief Process one byte through the tag-buffering state machine.
     * @param c Byte to process.
     * @version 2.0.2
     */
    void process_byte(char c);
};

} // namespace entropic
