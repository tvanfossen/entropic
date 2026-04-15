// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file session_logger.h
 * @brief Session model log — raw streaming transcript.
 *
 * Owns the FILE* for session_model.log, handles user/assistant
 * turn delimiters and raw token writes. Centralizes model log
 * lifecycle that was previously inline in the facade.
 *
 * @version 2.0.1
 */

#pragma once

#include <entropic/entropic_export.h>
#include <cstdio>
#include <filesystem>
#include <string>

namespace entropic {

/**
 * @brief Manages session_model.log for raw streaming content.
 *
 * Opened at configure time, used during run_streaming to record
 * the full user/assistant transcript. The session_model.log is
 * the unfiltered stream (includes think blocks, unlike session.log
 * which gets spdlog-formatted engine operations).
 *
 * @version 2.0.1
 */
class SessionLogger {
public:
    /**
     * @brief Construct with log directory.
     * @param log_dir Directory for session_model.log.
     * @version 2.0.1
     */
    ENTROPIC_EXPORT explicit SessionLogger(
        const std::filesystem::path& log_dir);

    /**
     * @brief Close the model log file.
     * @version 2.0.1
     */
    ENTROPIC_EXPORT ~SessionLogger();

    /// @brief Non-copyable.
    SessionLogger(const SessionLogger&) = delete;
    SessionLogger& operator=(const SessionLogger&) = delete;

    /**
     * @brief Log user input at the start of a turn.
     * @param input User input string.
     * @version 2.0.1
     */
    ENTROPIC_EXPORT void log_user_input(const std::string& input);

    /**
     * @brief Log a raw token from streaming output.
     * @param token Token data.
     * @param len Token length.
     * @version 2.0.1
     */
    ENTROPIC_EXPORT void log_raw_token(const char* token, size_t len);

    /**
     * @brief End the current assistant turn.
     * @version 2.0.1
     */
    ENTROPIC_EXPORT void end_turn();

    /**
     * @brief Check if the logger is open and writable.
     * @return true if FILE* is valid.
     * @version 2.0.1
     */
    ENTROPIC_EXPORT bool is_open() const;

    /**
     * @brief Static callback for StreamThinkFilter raw output.
     *
     * Matches the signature expected by StreamThinkFilter::set_raw_callback.
     * The user_data pointer must be a SessionLogger*.
     *
     * @param token Token data.
     * @param len Token length.
     * @param user_data SessionLogger pointer.
     * @callback
     * @version 2.0.1
     */
    static void raw_token_callback(
        const char* token, size_t len, void* user_data);

private:
    FILE* fp_ = nullptr; ///< Model log file handle
};

} // namespace entropic
