// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file conversation_state.h
 * @brief Persistent conversation state for the engine handle.
 *
 * Manages the message history across entropic_run calls.
 * System prompt is injected once on first use.
 *
 * @version 2.0.1
 */

#pragma once

#include <entropic/types/message.h>
#include <string>
#include <vector>

/**
 * @brief Conversation state owned by the engine handle.
 *
 * Tracks system prompt + user/assistant messages across turns.
 * The facade delegates all conversation management here.
 *
 * @internal
 * @version 2.0.1
 */
struct ConversationState {
    std::string system_prompt;                    ///< Cached system prompt
    std::vector<entropic::Message> messages;      ///< Full conversation history

    /**
     * @brief Append user message and return snapshot for engine.
     *
     * On first call, prepends system prompt. Returns a copy
     * since engine->run() may mutate.
     *
     * @param input User input string.
     * @return Full conversation snapshot [system, ...history, user].
     * @internal
     * @version 2.0.1
     */
    std::vector<entropic::Message> append_user(const char* input) {
        if (messages.empty()) {
            entropic::Message sys;
            sys.role = "system";
            sys.content = system_prompt;
            messages.push_back(std::move(sys));
        }
        entropic::Message usr;
        usr.role = "user";
        usr.content = input;
        messages.push_back(std::move(usr));
        return messages;  // copy
    }

    /**
     * @brief Append new messages from engine result.
     * @param result Full result from engine->run().
     * @param sent_len Number of messages sent (snapshot size).
     * @internal
     * @version 2.0.1
     */
    void append_result(const std::vector<entropic::Message>& result,
                       size_t sent_len) {
        for (size_t i = sent_len; i < result.size(); i++) {
            messages.push_back(result[i]);
        }
    }

    /**
     * @brief Clear all messages (new session).
     * @internal
     * @version 2.0.1
     */
    void clear() { messages.clear(); }

    /**
     * @brief Get message count.
     * @return Number of messages.
     * @internal
     * @version 2.0.1
     */
    size_t count() const { return messages.size(); }
};
