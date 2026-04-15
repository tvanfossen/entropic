// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file session_logger.cpp
 * @brief Session model log implementation.
 * @version 2.0.1
 */

#include <entropic/types/session_logger.h>
#include <entropic/types/logging.h>

static auto s_log = entropic::log::get("session");

namespace entropic {

/**
 * @brief Construct with log directory. Opens session_model.log for append.
 * @param log_dir Directory for session_model.log.
 * @internal
 * @version 2.0.1
 */
SessionLogger::SessionLogger(const std::filesystem::path& log_dir) {
    auto path = log_dir / "session_model.log";
    fp_ = fopen(path.string().c_str(), "a");
    if (!fp_) {
        s_log->warn("cannot open model log: {}", path.string());
    }
}

/**
 * @brief Close the model log file.
 * @internal
 * @version 2.0.1
 */
SessionLogger::~SessionLogger() {
    if (fp_) { fclose(fp_); }
}

/**
 * @brief Log user input at the start of a turn.
 * @param input User input string.
 * @internal
 * @version 2.0.1
 */
void SessionLogger::log_user_input(const std::string& input) {
    if (!fp_) { return; }
    fprintf(fp_, "--- USER ---\n%s\n--- ASSISTANT ---\n", input.c_str());
    fflush(fp_);
}

/**
 * @brief Log a raw token from streaming output.
 * @param token Token data.
 * @param len Token length.
 * @internal
 * @version 2.0.1
 */
void SessionLogger::log_raw_token(const char* token, size_t len) {
    if (!fp_ || !token || len == 0) { return; }
    fwrite(token, 1, len, fp_);
    fflush(fp_);
}

/**
 * @brief End the current assistant turn.
 * @internal
 * @version 2.0.1
 */
void SessionLogger::end_turn() {
    if (!fp_) { return; }
    fprintf(fp_, "\n\n");
    fflush(fp_);
}

/**
 * @brief Check if the logger is open and writable.
 * @return true if FILE* is valid.
 * @internal
 * @version 2.0.1
 */
bool SessionLogger::is_open() const {
    return fp_ != nullptr;
}

/**
 * @brief Static callback for StreamThinkFilter raw output.
 * @param token Token data.
 * @param len Token length.
 * @param user_data SessionLogger pointer.
 * @callback
 * @version 2.0.1
 */
void SessionLogger::raw_token_callback(
    const char* token, size_t len, void* user_data) {
    static_cast<SessionLogger*>(user_data)->log_raw_token(token, len);
}

} // namespace entropic
