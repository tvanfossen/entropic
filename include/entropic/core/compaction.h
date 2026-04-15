// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file compaction.h
 * @brief Auto-compaction for context management.
 *
 * Deterministic structured extraction (no model inference) to produce
 * predictable, compact briefings that preserve original task, tool call
 * history, and files touched.
 *
 * @version 1.8.4
 */

#pragma once

#include <entropic/types/config.h>
#include <entropic/types/message.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Track token usage across conversation.
 *
 * Uses simple heuristic (~4 chars per token) for estimation.
 * More accurate counting requires the model's tokenizer.
 *
 * @version 1.8.4
 */
class TokenCounter {
public:
    /**
     * @brief Construct a token counter.
     * @param max_tokens Maximum context window size.
     * @version 1.8.4
     */
    explicit TokenCounter(int max_tokens);

    /**
     * @brief Count tokens in a single message.
     * @param msg Message to count.
     * @return Estimated token count.
     * @version 1.8.4
     */
    int count_message(const Message& msg) const;

    /**
     * @brief Count total tokens in a message list.
     * @param messages Messages to count.
     * @return Total estimated token count.
     * @version 1.8.4
     */
    int count_messages(const std::vector<Message>& messages) const;

    /**
     * @brief Get usage as fraction of context window (0.0–1.0).
     * @param messages Messages to measure.
     * @return Usage fraction.
     * @version 1.8.4
     */
    float usage_percent(const std::vector<Message>& messages) const;

    /**
     * @brief Clear the token count cache.
     * @version 1.8.4
     */
    void clear_cache();

    int max_tokens; ///< Maximum context window size

private:
    /**
     * @brief Estimate token count for raw text.
     * @param text Input text.
     * @return Estimated token count.
     * @version 1.8.4
     */
    static int count_text(const std::string& text);

    mutable std::unordered_map<const void*, int> cache_; ///< Address-keyed cache
};

/**
 * @brief Result of a compaction operation.
 * @version 1.9.9 — extended from v1.8.4
 */
struct CompactionResult {
    bool compacted = false;        ///< Whether compaction occurred
    int old_token_count = 0;       ///< Token count before compaction
    int new_token_count = 0;       ///< Token count after compaction
    std::string summary;           ///< Generated summary text
    int preserved_messages = 0;    ///< Messages kept after compaction
    int messages_summarized = 0;   ///< Messages stripped into summary
    std::vector<Message> messages; ///< The compacted message list (v1.9.9)

    /* v1.9.9 additions */
    std::string identity;              ///< Identity that triggered compaction
    std::string compactor_source;      ///< "default", "global_custom", or identity name
    bool custom_compactor_used = false; ///< true if a consumer-provided compactor ran
};

/**
 * @brief Manages automatic context compaction.
 *
 * Monitors token usage and triggers deterministic summarization when
 * approaching the context limit. No model inference — uses structured
 * extraction only.
 *
 * @version 1.8.4
 */
class CompactionManager {
public:
    /**
     * @brief Construct a compaction manager.
     * @param config Compaction config (copied by value).
     * @param counter Token counter instance (shared reference).
     * @version 1.8.4
     */
    CompactionManager(const CompactionConfig& config, TokenCounter& counter);

    /**
     * @brief Check if compaction is needed and perform if so.
     * @param messages Current message list (modified in place if compacted).
     * @param force Bypass threshold check and compact immediately.
     * @param conversation_id Conversation ID for snapshot (empty = skip).
     * @return Compaction result.
     * @version 1.8.8
     */
    CompactionResult check_and_compact(
        std::vector<Message>& messages,
        bool force = false,
        const std::string& conversation_id = "");

    /**
     * @brief Compact messages using the value-density strategy.
     *
     * Public entry point for the default compaction algorithm. Used by
     * CompactorRegistry to wrap the built-in strategy as a compactor.
     * Does NOT check thresholds — callers are responsible for that.
     *
     * @param messages Messages to compact.
     * @return CompactionResult with compacted messages and metadata.
     * @version 1.9.9
     */
    CompactionResult compact_messages(
        const std::vector<Message>& messages);

    /**
     * @brief Set storage interface for compaction snapshots.
     * @param storage Storage callbacks (nullable).
     * @version 1.8.8
     */
    void set_storage(const struct StorageInterface* storage);

    CompactionConfig config; ///< Compaction configuration
    TokenCounter& counter;   ///< Shared token counter

private:
    /**
     * @brief Perform value-density compaction.
     * @param messages Input messages.
     * @param[out] summary Generated summary text.
     * @param[out] stripped_count Number of messages stripped.
     * @return Compacted message list.
     * @version 1.8.4
     */
    std::vector<Message> compact(
        const std::vector<Message>& messages,
        std::string& summary,
        int& stripped_count);

    /**
     * @brief Build structured summary from message history.
     * @param messages Messages to summarize.
     * @return Summary text.
     * @version 1.8.4
     */
    static std::string structured_summary(
        const std::vector<Message>& messages);

    /**
     * @brief Extract the original user task from messages.
     * @param messages Messages to search.
     * @return Task description string.
     * @version 1.8.4
     */
    static std::string extract_original_task(
        const std::vector<Message>& messages);

    /**
     * @brief Extract tool call log from messages.
     * @param messages Messages to scan.
     * @return Vector of (tool_name, brief_result) pairs.
     * @version 1.8.4
     */
    static std::vector<std::pair<std::string, std::string>>
    extract_tool_log(const std::vector<Message>& messages);

    /**
     * @brief Format summary as a compaction message.
     * @param summary Raw summary text.
     * @param message_count Number of original messages.
     * @return Formatted summary string.
     * @version 1.8.4
     */
    static std::string format_summary(
        const std::string& summary,
        int message_count);

    /**
     * @brief Save pre-compaction snapshot via storage interface.
     * @param conversation_id Conversation to snapshot.
     * @param messages Messages before compaction.
     * @version 1.8.8
     */
    void save_snapshot(const std::string& conversation_id,
                       const std::vector<Message>& messages);

    const struct StorageInterface* storage_ = nullptr; ///< Nullable storage (v1.8.8)
};

} // namespace entropic
