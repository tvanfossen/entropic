// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file compactor_registry.cpp
 * @brief Per-identity compactor registration and dispatch.
 * @version 1.9.9
 */

#include <entropic/core/compactor_registry.h>
#include <entropic/types/logging.h>

#include <sstream>

static auto logger = entropic::log::get("core.compactor_registry");

namespace entropic {

// ── JSON helpers (internal to this TU) ──────────────────

/**
 * @brief Map a character to its JSON escape sequence.
 * @param c Input character.
 * @return Escape string, or empty if no escaping needed.
 * @utility
 * @version 1.9.9
 */
static const char* json_escape_char(char c) {
    const char* esc = nullptr;
    switch (c) {
    case '"':  esc = "\\\""; break;
    case '\\': esc = "\\\\"; break;
    case '\n': esc = "\\n";  break;
    case '\r': esc = "\\r";  break;
    case '\t': esc = "\\t";  break;
    default: break;
    }
    return esc;
}

/**
 * @brief Escape a string for JSON embedding.
 * @param input Raw string.
 * @return JSON-safe escaped string.
 * @utility
 * @version 1.9.9
 */
static std::string json_escape(const std::string& input) {
    std::ostringstream oss;
    for (char c : input) {
        const char* esc = json_escape_char(c);
        if (esc != nullptr) { oss << esc; }
        else { oss << c; }
    }
    return oss.str();
}

/**
 * @brief Serialize messages to minimal JSON array.
 * @param messages Messages to serialize.
 * @return JSON array string.
 * @utility
 * @version 1.9.9
 */
static std::string serialize_messages(
    const std::vector<Message>& messages) {
    std::ostringstream oss;
    oss << '[';
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i > 0) oss << ',';
        oss << "{\"role\":\"" << messages[i].role
            << "\",\"content\":\""
            << json_escape(messages[i].content) << "\"}";
    }
    oss << ']';
    return oss.str();
}

/**
 * @brief Serialize compaction config + identity to JSON.
 * @param config Compaction config.
 * @param identity Identity name.
 * @param token_count Current token count (0 if unknown).
 * @return JSON object string.
 * @utility
 * @version 1.9.9
 */
static std::string serialize_config(
    const CompactionConfig& config,
    const std::string& identity,
    int token_count) {
    std::ostringstream oss;
    oss << "{\"identity\":\"" << json_escape(identity) << "\""
        << ",\"token_count\":" << token_count
        << ",\"max_tokens\":0"
        << ",\"threshold_percent\":" << config.threshold_percent
        << ",\"force\":true}";
    return oss.str();
}

// ── Minimal JSON parser for message arrays ──────────────

/**
 * @brief Lightweight cursor into a JSON string.
 * @internal
 * @version 1.9.9
 */
struct JsonCursor {
    const std::string& data; ///< Source string
    size_t pos = 0;          ///< Current position
};

/**
 * @brief Skip whitespace in a JSON cursor.
 * @param c JSON cursor.
 * @utility
 * @version 1.9.9
 */
static void json_skip_ws(JsonCursor& c) {
    while (c.pos < c.data.size() && isspace(c.data[c.pos])) {
        ++c.pos;
    }
}

/**
 * @brief Decode one JSON escape sequence character.
 * @param ch The character after the backslash.
 * @return Decoded character.
 * @utility
 * @version 1.9.9
 */
static char json_unescape_char(char ch) {
    char result = ch;
    switch (ch) {
    case '"':  result = '"';  break;
    case '\\': result = '\\'; break;
    case 'n':  result = '\n'; break;
    case 'r':  result = '\r'; break;
    case 't':  result = '\t'; break;
    default: break;
    }
    return result;
}

/**
 * @brief Read a JSON quoted string from cursor.
 * @param c JSON cursor (positioned at or before opening quote).
 * @param[out] out Decoded string.
 * @return true on success.
 * @utility
 * @version 1.9.9
 */
static bool json_read_string(JsonCursor& c, std::string& out) {
    json_skip_ws(c);
    if (c.pos >= c.data.size() || c.data[c.pos] != '"') {
        return false;
    }
    ++c.pos;
    out.clear();
    while (c.pos < c.data.size() && c.data[c.pos] != '"') {
        if (c.data[c.pos] == '\\' && c.pos + 1 < c.data.size()) {
            out += json_unescape_char(c.data[c.pos + 1]);
            c.pos += 2;
        } else {
            out += c.data[c.pos++];
        }
    }
    if (c.pos >= c.data.size()) return false;
    ++c.pos; // skip closing '"'
    return true;
}

/**
 * @brief Skip a JSON array or object in cursor (nested-aware).
 * @param c JSON cursor (positioned at opening '[' or '{').
 * @utility
 * @version 1.9.11
 */
static void json_skip_composite(JsonCursor& c) {
    char open = c.data[c.pos];
    char close = (open == '[') ? ']' : '}';
    int depth = 1;
    ++c.pos;
    while (c.pos < c.data.size() && depth > 0) {
        char ch = c.data[c.pos];
        if (ch == '"') {
            std::string dummy;
            json_read_string(c, dummy);
            continue;
        }
        if (ch == open) ++depth;
        else if (ch == close) --depth;
        ++c.pos;
    }
}

/**
 * @brief Skip one JSON value (string, primitive, array, or object).
 * @param c JSON cursor.
 * @utility
 * @version 1.9.11
 */
static void json_skip_value(JsonCursor& c) {
    json_skip_ws(c);
    if (c.pos >= c.data.size()) return;
    char ch = c.data[c.pos];
    if (ch == '"') {
        std::string dummy;
        json_read_string(c, dummy);
        return;
    }
    if (ch == '[' || ch == '{') {
        json_skip_composite(c);
        return;
    }
    while (c.pos < c.data.size()
           && c.data[c.pos] != ',' && c.data[c.pos] != '}'
           && c.data[c.pos] != ']') {
        ++c.pos;
    }
}

/**
 * @brief Read one field of a content part object into type/text.
 * @param c JSON cursor (positioned at key start).
 * @param[out] type_val Populated if key == "type".
 * @param[out] text_val Populated if key == "text".
 * @return true on success.
 * @utility
 * @version 1.9.11
 */
static bool json_read_content_part_field(JsonCursor& c,
                                         std::string& type_val,
                                         std::string& text_val) {
    std::string key;
    if (!json_read_string(c, key)) return false;
    json_skip_ws(c);
    if (c.pos >= c.data.size() || c.data[c.pos] != ':') return false;
    ++c.pos;
    if (key == "type") { json_read_string(c, type_val); }
    else if (key == "text") { json_read_string(c, text_val); }
    else { json_skip_value(c); }
    return true;
}

/**
 * @brief Parse one content part object, extract type and text.
 * @param c JSON cursor (positioned after opening '{').
 * @param[out] type_val The "type" field value.
 * @param[out] text_val The "text" field value.
 * @return true on success (object fully consumed).
 * @utility
 * @version 1.9.11
 */
static bool json_parse_content_part(JsonCursor& c,
                                    std::string& type_val,
                                    std::string& text_val) {
    while (c.pos < c.data.size() && c.data[c.pos] != '}') {
        json_skip_ws(c);
        if (c.data[c.pos] == ',') { ++c.pos; continue; }
        if (!json_read_content_part_field(c, type_val, text_val)) {
            return false;
        }
    }
    if (c.pos < c.data.size()) ++c.pos; // skip '}'
    return true;
}

/**
 * @brief Append text from a content part if type == "text".
 * @param type_val Parsed type string.
 * @param text_val Parsed text string.
 * @param[out] text Accumulated text.
 * @utility
 * @version 1.9.11
 */
static void append_text_part(const std::string& type_val,
                             const std::string& text_val,
                             std::string& text) {
    if (type_val != "text" || text_val.empty()) return;
    if (!text.empty()) text += ' ';
    text += text_val;
}

/**
 * @brief Read one content part object from an array cursor.
 *
 * Expects cursor at element start ('{' or ',' or ']'). Skips
 * commas, parses the object, appends text to output if type=text.
 *
 * @param c JSON cursor.
 * @param[out] text Accumulated text.
 * @return 1 = continue, 0 = array end reached, -1 = error.
 * @utility
 * @version 1.9.11
 */
/**
 * @brief Advance cursor past commas/whitespace to next element.
 * @param c JSON cursor.
 * @return Character at next element, or '\0' if past end.
 * @utility
 * @version 1.9.11
 */
static char json_skip_to_element(JsonCursor& c) {
    while (c.pos < c.data.size()) {
        json_skip_ws(c);
        char ch = c.data[c.pos];
        if (ch != ',') return ch;
        ++c.pos;
    }
    return '\0';
}

/**
 * @brief Parse one content part object and append text if applicable.
 * @param c JSON cursor (positioned at '{').
 * @param[out] text Accumulated text output.
 * @return true on success.
 * @utility
 * @version 1.9.11
 */
static bool json_read_one_content_part(JsonCursor& c,
                                       std::string& text) {
    ++c.pos; // skip '{'
    std::string type_val;
    std::string text_val;
    bool ok = json_parse_content_part(c, type_val, text_val);
    if (ok) append_text_part(type_val, text_val, text);
    return ok;
}

/**
 * @brief Extract concatenated text from a JSON content array.
 *
 * Scans for {"type":"text","text":"..."} objects within a content
 * array and joins their text values. Skips image parts.
 *
 * @param c JSON cursor (positioned at opening '[').
 * @param[out] text Extracted text.
 * @return true on success.
 * @utility
 * @version 1.9.11
 */
static bool json_extract_text_from_array(JsonCursor& c,
                                         std::string& text) {
    if (c.pos >= c.data.size() || c.data[c.pos] != '[') {
        return false;
    }
    ++c.pos; // skip '['
    text.clear();

    bool ok = true;
    char ch = json_skip_to_element(c);
    while (ok && ch == '{') {
        ok = json_read_one_content_part(c, text);
        ch = ok ? json_skip_to_element(c) : '\0';
    }
    if (ok && ch == ']') { ++c.pos; }
    return ok && ch == ']';
}

/**
 * @brief Read one key:value pair and apply to message fields.
 *
 * Handles both string content and array content (multimodal).
 * For array content, extracts text parts only.
 *
 * @param c JSON cursor (positioned at key start).
 * @param[out] msg Message to populate.
 * @return true on success.
 * @utility
 * @version 1.9.11
 */
static bool json_read_field(JsonCursor& c, Message& msg) {
    std::string key;
    bool ok = json_read_string(c, key);
    json_skip_ws(c);
    if (ok && c.pos < c.data.size() && c.data[c.pos] == ':') {
        ++c.pos;
    } else {
        ok = false;
    }

    if (!ok) return false;

    if (key == "role") {
        ok = json_read_string(c, msg.role);
    } else if (key == "content") {
        json_skip_ws(c);
        if (c.pos < c.data.size() && c.data[c.pos] == '[') {
            ok = json_extract_text_from_array(c, msg.content);
        } else {
            ok = json_read_string(c, msg.content);
        }
    } else {
        json_skip_value(c);
    }
    return ok;
}

/**
 * @brief Parse one JSON object into a Message (role + content).
 * @param c JSON cursor (positioned after opening '{').
 * @param[out] msg Output message.
 * @return true on success.
 * @utility
 * @version 1.9.9
 */
static bool json_parse_message(JsonCursor& c, Message& msg) {
    while (c.pos < c.data.size()) {
        json_skip_ws(c);
        if (c.data[c.pos] == '}') { ++c.pos; return true; }
        if (c.data[c.pos] == ',') { ++c.pos; continue; }
        if (!json_read_field(c, msg)) return false;
    }
    return false;
}

/**
 * @brief Parse message objects from a JSON array cursor.
 * @param c JSON cursor (positioned after opening '[').
 * @param[out] messages Output message vector.
 * @return true on success.
 * @utility
 * @version 1.9.9
 */
static bool json_parse_array(JsonCursor& c,
                             std::vector<Message>& messages) {
    bool ok = true;
    while (ok && c.pos < c.data.size()) {
        json_skip_ws(c);
        if (c.data[c.pos] == ']') break;
        if (c.data[c.pos] == ',') { ++c.pos; continue; }
        ok = (c.data[c.pos] == '{');
        if (!ok) break;
        ++c.pos;

        Message msg;
        ok = json_parse_message(c, msg);
        if (ok) messages.push_back(std::move(msg));
    }
    return ok;
}

/**
 * @brief Parse a JSON array of message objects.
 * @param json JSON array string.
 * @param[out] messages Output message vector.
 * @return true on success, false on parse error.
 * @utility
 * @version 1.9.9
 */
static bool parse_messages(const char* json,
                           std::vector<Message>& messages) {
    if (json == nullptr || json[0] != '[') return false;
    messages.clear();
    std::string input(json);
    JsonCursor c{input, 1};
    return json_parse_array(c, messages);
}

// ── CompactorRegistry ───────────────────────────────────

/**
 * @brief Construct with default compactor wrapping CompactionManager.
 * @param default_manager v1.8.4 CompactionManager reference.
 * @internal
 * @version 1.9.9
 */
CompactorRegistry::CompactorRegistry(
    CompactionManager& default_manager)
    : default_manager_(default_manager) {
    default_compactor_ = [&mgr = default_manager_](
        const std::vector<Message>& messages,
        const CompactionConfig& /*config*/,
        const std::string& /*identity*/) {
        return mgr.compact_messages(messages);
    };
}

/**
 * @brief Register a compactor for a specific identity.
 * @param identity Identity name ("" for global fallback).
 * @param compactor C function pointer.
 * @param user_data Opaque pointer.
 * @return ENTROPIC_OK or ENTROPIC_ERROR_INVALID_CONFIG.
 * @internal
 * @version 1.9.9
 */
entropic_error_t CompactorRegistry::register_compactor(
    const std::string& identity,
    entropic_compactor_fn compactor,
    void* user_data) {
    if (compactor == nullptr) {
        logger->error("NULL compactor for identity '{}'", identity);
        return ENTROPIC_ERROR_INVALID_CONFIG;
    }

    CompactorEntry entry;
    entry.c_callback = compactor;
    entry.user_data = user_data;
    entry.cpp_fn = wrap_c_compactor(compactor, user_data);

    std::unique_lock lock(mutex_);
    compactors_[identity] = std::move(entry);

    logger->info("Registered compactor for identity '{}'",
                 identity.empty() ? "(global)" : identity);
    return ENTROPIC_OK;
}

/**
 * @brief Deregister a compactor for a specific identity.
 * @param identity Identity name ("" for global).
 * @return ENTROPIC_OK (idempotent).
 * @internal
 * @version 1.9.9
 */
entropic_error_t CompactorRegistry::deregister_compactor(
    const std::string& identity) {
    std::unique_lock lock(mutex_);
    compactors_.erase(identity);

    logger->info("Deregistered compactor for identity '{}'",
                 identity.empty() ? "(global)" : identity);
    return ENTROPIC_OK;
}

/**
 * @brief Resolve and call the appropriate compactor.
 * @param identity Current identity name.
 * @param messages Messages to compact.
 * @param config Compaction configuration.
 * @return CompactionResult with metadata.
 * @internal
 * @version 1.9.9
 */
CompactionResult CompactorRegistry::compact(
    const std::string& identity,
    const std::vector<Message>& messages,
    const CompactionConfig& config) {
    CompactorFn selected;
    std::string source;
    bool is_custom = false;

    {   // Snapshot under read lock, release before calling
        std::shared_lock lock(mutex_);
        auto it = compactors_.find(identity);
        if (it != compactors_.end()) {
            selected = it->second.cpp_fn;
            source = identity;
            is_custom = true;
        } else {
            it = compactors_.find("");
            if (it != compactors_.end()) {
                selected = it->second.cpp_fn;
                source = "global_custom";
                is_custom = true;
            }
        }
    }

    if (!is_custom) {
        return run_default(identity, messages, config);
    }
    return run_custom(
        selected, source, identity, messages, config);
}

/**
 * @brief Run the built-in default compactor.
 * @param identity Identity name for result metadata.
 * @param messages Messages to compact.
 * @param config Compaction configuration.
 * @return CompactionResult from default strategy.
 * @internal
 * @version 1.9.9
 */
CompactionResult CompactorRegistry::run_default(
    const std::string& identity,
    const std::vector<Message>& messages,
    const CompactionConfig& config) {
    auto result = default_compactor_(messages, config, identity);
    result.identity = identity;
    result.compactor_source = "default";
    result.custom_compactor_used = false;
    return result;
}

/**
 * @brief Run a custom compactor with fallback to default.
 * @param selected Custom compactor function.
 * @param source Compactor source label.
 * @param identity Identity name.
 * @param messages Messages to compact.
 * @param config Compaction configuration.
 * @return CompactionResult from custom or fallback default.
 * @internal
 * @version 1.9.9
 */
CompactionResult CompactorRegistry::run_custom(
    const CompactorFn& selected,
    const std::string& source,
    const std::string& identity,
    const std::vector<Message>& messages,
    const CompactionConfig& config) {
    auto result = selected(messages, config, identity);

    if (result.compacted) {
        result.identity = identity;
        result.compactor_source = source;
        result.custom_compactor_used = true;
        return result;
    }

    logger->warn("Custom compactor '{}' failed for '{}', "
                 "falling back to default", source, identity);
    return run_default(identity, messages, config);
}

/**
 * @brief Check if a custom compactor is registered for an identity.
 * @param identity Identity name.
 * @return true if per-identity or global custom registered.
 * @internal
 * @version 1.9.9
 */
bool CompactorRegistry::has_custom_compactor(
    const std::string& identity) const {
    std::shared_lock lock(mutex_);
    if (compactors_.count(identity) > 0) {
        return true;
    }
    return compactors_.count("") > 0;
}

/**
 * @brief Wrap a C function pointer into a CompactorFn.
 * @param compactor C function pointer.
 * @param user_data Opaque pointer.
 * @return C++ callable wrapping the C callback.
 * @internal
 * @version 1.9.9
 */
CompactorFn CompactorRegistry::wrap_c_compactor(
    entropic_compactor_fn compactor,
    void* user_data) {
    return [compactor, user_data](
        const std::vector<Message>& messages,
        const CompactionConfig& config,
        const std::string& identity) -> CompactionResult {
        auto msg_json = serialize_messages(messages);
        auto cfg_json = serialize_config(config, identity, 0);

        char* out_messages = nullptr;
        char* out_summary = nullptr;
        int rc = compactor(
            msg_json.c_str(), cfg_json.c_str(),
            &out_messages, &out_summary, user_data);

        CompactionResult result;
        if (rc != 0) {
            result.compacted = false;
            free(out_messages);
            free(out_summary);
            return result;
        }

        result.compacted = true;
        if (out_summary != nullptr) {
            result.summary = out_summary;
            free(out_summary);
        }
        if (out_messages != nullptr) {
            parse_messages(out_messages, result.messages);
            result.preserved_messages =
                static_cast<int>(result.messages.size());
            free(out_messages);
        }
        return result;
    };
}

} // namespace entropic
