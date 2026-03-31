/**
 * @file compaction.cpp
 * @brief Auto-compaction implementation.
 * @version 1.8.4
 */

#include <entropic/core/compaction.h>
#include <entropic/core/engine_types.h>
#include <entropic/types/logging.h>

#include <sstream>

static auto logger = entropic::log::get("core.compaction");

namespace entropic {

// ── TokenCounter ─────────────────────────────────────────

/**
 * @brief Construct a token counter.
 * @param max_tokens Maximum context window size.
 * @internal
 * @version 1.8.4
 */
TokenCounter::TokenCounter(int max_tokens)
    : max_tokens(max_tokens) {}

/**
 * @brief Estimate token count for raw text (~4 chars/token).
 * @param text Input text.
 * @return Estimated token count.
 * @internal
 * @version 1.8.4
 */
int TokenCounter::count_text(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    return static_cast<int>(text.size()) / 4 + 1;
}

/**
 * @brief Count tokens in a single message.
 * @param msg Message to count.
 * @return Estimated token count (content + role overhead).
 * @internal
 * @version 1.8.4
 */
int TokenCounter::count_message(const Message& msg) const {
    const auto* key = static_cast<const void*>(&msg);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }
    int count = count_text(msg.content) + 4; // +4 for role tokens
    cache_[key] = count;
    return count;
}

/**
 * @brief Count total tokens in a message list.
 * @param messages Messages to count.
 * @return Total estimated token count.
 * @internal
 * @version 1.8.4
 */
int TokenCounter::count_messages(
    const std::vector<Message>& messages) const {
    int total = 0;
    for (const auto& msg : messages) {
        total += count_message(msg);
    }
    return total;
}

/**
 * @brief Get usage as fraction of context window.
 * @param messages Messages to measure.
 * @return Usage fraction (0.0 if max_tokens is 0).
 * @internal
 * @version 1.8.4
 */
float TokenCounter::usage_percent(
    const std::vector<Message>& messages) const {
    if (max_tokens == 0) {
        return 0.0f;
    }
    return static_cast<float>(count_messages(messages))
         / static_cast<float>(max_tokens);
}

/**
 * @brief Clear the token count cache.
 * @internal
 * @version 1.8.4
 */
void TokenCounter::clear_cache() {
    cache_.clear();
}

// ── CompactionManager ────────────────────────────────────

/**
 * @brief Construct a compaction manager.
 * @param config Compaction configuration.
 * @param counter Token counter (shared reference).
 * @internal
 * @version 1.8.4
 */
CompactionManager::CompactionManager(
    const CompactionConfig& config,
    TokenCounter& counter)
    : config(config), counter(counter) {}

/**
 * @brief Check and perform compaction if over threshold.
 * @param messages Message list (modified in place if compacted).
 * @param force Bypass threshold.
 * @return CompactionResult with before/after stats.
 * @internal
 * @version 1.8.8
 */
CompactionResult CompactionManager::check_and_compact(
    std::vector<Message>& messages,
    bool force,
    const std::string& conversation_id) {
    int current = counter.count_messages(messages);
    int threshold = static_cast<int>(
        static_cast<float>(counter.max_tokens) * config.threshold_percent);

    if (!force && current < threshold) {
        return {false, current, current};
    }

    if (!config.enabled) {
        logger->warn("Context at {}/{} tokens, compaction disabled",
                     current, counter.max_tokens);
        return {false, current, current};
    }

    logger->info("Compacting conversation ({} tokens)", current);

    // Save full history before compacting (v1.8.8)
    if (config.save_full_history && !conversation_id.empty()) {
        save_snapshot(conversation_id, messages);
    }

    std::string summary;
    int stripped = 0;
    auto compacted = compact(messages, summary, stripped);

    counter.clear_cache();
    int new_count = counter.count_messages(compacted);

    if (new_count >= current) {
        logger->error("Compaction did not reduce tokens: {} -> {}",
                      current, new_count);
    }

    logger->info("Compacted {} -> {} tokens", current, new_count);
    messages = std::move(compacted);

    CompactionResult result;
    result.compacted = true;
    result.old_token_count = current;
    result.new_token_count = new_count;
    result.summary = summary;
    result.preserved_messages = static_cast<int>(messages.size()) - 1;
    result.messages_summarized = stripped;
    return result;
}

/**
 * @brief Perform value-density compaction.
 * @param messages Input messages.
 * @param summary Output: generated summary.
 * @param stripped_count Output: messages stripped.
 * @return Compacted message list.
 * @internal
 * @version 1.8.4
 */
std::vector<Message> CompactionManager::compact(
    const std::vector<Message>& messages,
    std::string& summary,
    int& stripped_count) {
    Message const* system_msg = nullptr;
    size_t start = 0;
    if (!messages.empty() && messages[0].role == "system") {
        system_msg = &messages[0];
        start = 1;
    }

    std::vector<const Message*> user_msgs;
    std::vector<const Message*> assistant_msgs;
    stripped_count = 0;

    for (size_t i = start; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        auto src = msg.metadata.find("source");
        if (src != msg.metadata.end() && src->second == "user") {
            user_msgs.push_back(&msg);
        } else if (msg.role == "assistant") {
            assistant_msgs.push_back(&msg);
        } else {
            ++stripped_count;
        }
    }

    std::vector<Message> working(messages.begin() + static_cast<long>(start),
                                 messages.end());
    summary = structured_summary(working);
    Message summary_msg;
    summary_msg.role = "user";
    summary_msg.content = format_summary(
        summary, static_cast<int>(working.size()));

    std::vector<Message> result;
    if (system_msg != nullptr) {
        result.push_back(*system_msg);
    }
    result.push_back(std::move(summary_msg));
    for (const auto* m : user_msgs) {
        result.push_back(*m);
    }
    if (!assistant_msgs.empty()) {
        result.push_back(*assistant_msgs.back());
    }
    return result;
}

/**
 * @brief Build deterministic structured summary from history.
 * @param messages Messages to summarize.
 * @return Summary text.
 * @internal
 * @version 1.8.4
 */
std::string CompactionManager::structured_summary(
    const std::vector<Message>& messages) {
    std::string lines = "Original task: "
                      + extract_original_task(messages);

    auto tool_log = extract_tool_log(messages);
    if (!tool_log.empty()) {
        lines += "\n\nTool calls made (oldest first):";
        for (const auto& [name, brief] : tool_log) {
            lines += "\n- " + name + ": " + brief;
        }
    }
    return lines;
}

/**
 * @brief Extract original user task from messages.
 * @param messages Messages to search.
 * @return Task text (truncated to 500 chars).
 * @internal
 * @version 1.8.4
 */
std::string CompactionManager::extract_original_task(
    const std::vector<Message>& messages) {
    std::string task;
    for (const auto& msg : messages) {
        auto src = msg.metadata.find("source");
        if (src != msg.metadata.end() && src->second == "user") {
            task = msg.content;
            break;
        }
    }
    if (task.empty()) {
        for (const auto& msg : messages) {
            if (msg.role == "user" && !msg.content.empty()
                && msg.content[0] != '[') {
                task = msg.content;
                break;
            }
        }
    }
    if (task.empty()) {
        return "(no user message found)";
    }
    if (task.size() > 500) {
        return task.substr(0, 500) + "...";
    }
    return task;
}

/**
 * @brief Extract tool call log from messages.
 * @param messages Messages to scan.
 * @return Vector of (name, brief_result) pairs.
 * @internal
 * @version 1.8.4
 */
std::vector<std::pair<std::string, std::string>>
CompactionManager::extract_tool_log(
    const std::vector<Message>& messages) {
    std::vector<std::pair<std::string, std::string>> log;
    for (const auto& msg : messages) {
        auto it = msg.metadata.find("tool_name");
        if (it == msg.metadata.end()) {
            continue;
        }
        const auto& name = it->second;
        if (msg.content.rfind("[Previous:", 0) == 0) {
            log.emplace_back(name, "(pruned)");
            continue;
        }
        auto nl = msg.content.find('\n');
        std::string brief = msg.content.substr(0, std::min(nl, size_t{100}));
        log.emplace_back(name, brief);
    }
    return log;
}

/**
 * @brief Format summary as a compaction message.
 * @param summary Raw summary text.
 * @param message_count Number of original messages.
 * @return Formatted summary string.
 * @internal
 * @version 1.8.4
 */
std::string CompactionManager::format_summary(
    const std::string& summary,
    int message_count) {
    return "[CONVERSATION SUMMARY]\n"
           "The following summarizes "
         + std::to_string(message_count)
         + " previous messages that have been compacted"
           " to save context space.\n\n"
         + summary
         + "\n\n[END SUMMARY - Recent conversation continues below]";
}

/**
 * @brief Compact messages using value-density strategy.
 *
 * Public wrapper around the private compact() method. Returns a
 * fully-populated CompactionResult including the compacted message
 * list. Does not check thresholds.
 *
 * @param messages Messages to compact.
 * @return CompactionResult with compacted messages and metadata.
 * @internal
 * @version 1.9.9
 */
CompactionResult CompactionManager::compact_messages(
    const std::vector<Message>& messages) {
    int old_count = counter.count_messages(messages);

    std::string summary;
    int stripped = 0;
    auto compacted = compact(messages, summary, stripped);

    counter.clear_cache();
    int new_count = counter.count_messages(compacted);

    CompactionResult result;
    result.compacted = true;
    result.old_token_count = old_count;
    result.new_token_count = new_count;
    result.summary = summary;
    result.preserved_messages =
        static_cast<int>(compacted.size()) - 1;
    result.messages_summarized = stripped;
    result.messages = compacted;
    result.compactor_source = "default";
    return result;
}

/**
 * @brief Set storage interface for compaction snapshots.
 * @param storage Storage callbacks (nullable).
 * @internal
 * @version 1.8.8
 */
void CompactionManager::set_storage(const StorageInterface* storage) {
    storage_ = storage;
}

/**
 * @brief Save pre-compaction snapshot via storage interface.
 * @param conversation_id Conversation to snapshot.
 * @param messages Messages before compaction.
 * @internal
 * @version 1.8.8
 */
/**
 * @brief Escape a string for JSON embedding.
 * @param input Raw string.
 * @return JSON-safe escaped string.
 * @utility
 * @version 1.8.8
 */
static std::string json_escape(const std::string& input) {
    std::ostringstream oss;
    for (char c : input) {
        if (c == '"') oss << "\\\"";
        else if (c == '\\') oss << "\\\\";
        else if (c == '\n') oss << "\\n";
        else oss << c;
    }
    return oss.str();
}

/**
 * @brief Serialize messages to minimal JSON array.
 * @param messages Messages to serialize.
 * @return JSON array string.
 * @utility
 * @version 1.8.8
 */
static std::string serialize_messages_json(
        const std::vector<Message>& messages) {
    std::ostringstream oss;
    oss << '[';
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i > 0) oss << ',';
        oss << "{\"role\":\"" << messages[i].role
            << "\",\"content\":\"" << json_escape(messages[i].content)
            << "\"}";
    }
    oss << ']';
    return oss.str();
}

/**
 * @brief Save pre-compaction snapshot via storage interface.
 * @param conversation_id Conversation to snapshot.
 * @param messages Messages before compaction.
 * @internal
 * @version 1.8.8
 */
void CompactionManager::save_snapshot(
    const std::string& conversation_id,
    const std::vector<Message>& messages) {
    if (!storage_ || !storage_->save_snapshot) {
        return;
    }

    auto json_str = serialize_messages_json(messages);
    storage_->save_snapshot(
        conversation_id.c_str(), json_str.c_str(),
        storage_->user_data);
    logger->info("Saved compaction snapshot for {} ({} messages)",
                 conversation_id, messages.size());
}

} // namespace entropic
