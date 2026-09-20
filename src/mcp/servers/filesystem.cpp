// SPDX-License-Identifier: Apache-2.0
/**
 * @file filesystem.cpp
 * @brief Filesystem MCP server — read/write/edit/glob/grep/list_directory.
 *
 * Implements 6 tools as ToolBase subclasses, a FileAccessTracker for
 * read-before-write enforcement, and the FilesystemServer that owns
 * and registers all tools.
 *
 * @version 1.8.5
 */

#include <entropic/mcp/servers/filesystem.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

static auto logger = entropic::log::get("mcp.filesystem");

namespace entropic {

// ── FileAccessTracker ────────────────────────────────────

/**
 * @brief Record that a file was read with its content hash.
 *
 * The tracker is what makes read-before-write enforceable: the hash
 * stored here is compared at write time to detect external
 * modification.
 *
 * @param path Canonical file path.
 * @param hash Content hash at time of read.
 * @req REQ-MCP-021
 * @version 1.8.5
 */
void FileAccessTracker::record_read(const std::string& path,
                                    size_t hash) {
    reads_[path] = hash;
    logger->info("Tracked read: {}", path);
}


/**
 * @brief Check if a file was ever read in this session.
 * @param path Canonical file path.
 * @return true when a read was recorded for this path, regardless of
 *         whether the content has since changed.
 * @req REQ-MCP-021
 * @version 1.8.5
 */
bool FileAccessTracker::was_read(const std::string& path) const {
    return reads_.count(path) > 0;
}

// ── File-local helpers ───────────────────────────────────

namespace {

/**
 * @brief Check if a directory name should be skipped.
 *
 * gh#161: the list itself now lives on IgnoreMatcher, because the
 * `.gitignore` DISCOVERY walk has to honour exactly the same names.
 * Two private copies were how discovery came to walk `.git` while
 * glob/grep pruned it.
 *
 * @param name Directory name to check.
 * @return true if name is in the skip list.
 * @dg_internal
 * @version 2.13.0
 */
bool should_skip_dir(const std::string& name) {
    return IgnoreMatcher::is_skipped_dir_name(name);
}

/**
 * @brief Entry budget for one glob/grep tree walk (gh#161).
 *
 * The pre-2.13.0 caps bounded MATCHES, not work: a pattern matching
 * nothing still visited every entry in the tree. This bounds the walk
 * itself and records that it was cut short, so the result can say so.
 *
 * @dg_internal
 * @version 2.13.0
 */
struct WalkBudget {
    int max_entries = 0;   ///< <= 0 means unbounded
    long visited = 0;      ///< Entries visited so far
    bool truncated = false; ///< Walk stopped on the cap
};

/**
 * @brief Charge one visited entry to the budget.
 * @param[in,out] budget Walk budget.
 * @return true when the cap is now spent and the walk must stop.
 * @dg_internal
 * @version 2.13.0
 */
bool walk_budget_spent(WalkBudget& budget) {
    ++budget.visited;
    bool spent = budget.max_entries > 0
        && budget.visited >= static_cast<long>(budget.max_entries);
    if (spent) { budget.truncated = true; }
    return spent;
}

/**
 * @brief The truncation sentinel appended to a cut-short result.
 *
 * Appended as a trailing element rather than wrapping the array,
 * because glob and grep results are arrays of paths and match objects
 * respectively and both shapes are already contracted. A truncation
 * the model cannot see would be worse than the 87 s hang it replaces —
 * it would look like a complete, empty answer.
 *
 * @param budget Spent walk budget.
 * @return JSON object carrying the human-readable notice.
 * @dg_internal
 * @version 2.13.0
 */
json walk_truncation_notice(const WalkBudget& budget) {
    json note;
    note["truncated"] = true;
    note["note"] = "walk truncated at " + std::to_string(budget.visited)
        + " entries — narrow the pattern";
    return note;
}

/**
 * @brief Append the truncation sentinel when the walk was cut short.
 *
 * Shared by glob and grep so the two can never disagree about how a
 * bounded walk is reported (gh#161).
 *
 * @param[in,out] result Result array to annotate.
 * @param budget Walk budget after the walk.
 * @param tool Tool name, for the log line.
 * @param pattern Requested pattern, for the log line.
 * @dg_internal
 * @version 2.13.0
 */
void note_truncated_walk(json& result, const WalkBudget& budget,
                         const std::string& tool,
                         const std::string& pattern) {
    if (!budget.truncated) { return; }
    auto note = walk_truncation_notice(budget);
    logger->warn("{} '{}': {}", tool, pattern,
                 note.at("note").get<std::string>());
    result.push_back(note);
}

/**
 * @brief Read entire file contents into a string.
 * @param path File path.
 * @return File contents.
 * @throws std::runtime_error if file cannot be opened.
 * @dg_internal
 * @version 1.8.5
 */
std::string read_file_contents(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error(
            "Cannot open file: " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/**
 * @brief Write string contents to a file, creating parent dirs.
 * @param path File path.
 * @param content Content to write.
 * @dg_internal
 * @version 1.8.5
 */
void write_file_contents(const fs::path& path,
                         const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        throw std::runtime_error(
            "Cannot write file: " + path.string());
    }
    out << content;
}

/**
 * @brief Hash a string using std::hash.
 * @param s String to hash.
 * @return Hash value.
 * @dg_internal
 * @version 1.8.5
 */
size_t hash_content(const std::string& s) {
    return std::hash<std::string>{}(s);
}

/**
 * @brief Build a JSON error response.
 *
 * Every filesystem failure is a structured object rather than prose, so
 * the model can branch on the code: not_found, is_directory, ignored,
 * size_exceeded, read_before_write, multiple_matches, invalid_regex,
 * not_directory.
 *
 * @param code Error code string.
 * @param message Human-readable message, which for not_found names the
 *                tool to use instead (gh#124).
 * @return `{"error":"<code>","message":"<message>"}` as a JSON string.
 * @req REQ-MCP-021
 * @version 1.8.5
 */
std::string make_error(const std::string& code,
                       const std::string& message) {
    json j;
    j["error"] = code;
    j["message"] = message;
    return j.dump();
}

/**
 * @brief Build read_file result JSON from content string.
 * @param path Canonical path string.
 * @param content File content.
 * @return JSON carrying the path, the total line count, and `lines` as
 *         an ORDERED ARRAY — not a keyed object (gh#120), so line order
 *         survives serialisation.
 * @req REQ-MCP-021
 * @version 2.9.18
 */
std::string build_read_result(const std::string& path,
                              const std::string& content) {
    json result;
    result["path"] = path;

    std::istringstream stream(content);
    std::string line;
    json lines = json::array();

    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    result["total"] = static_cast<int>(lines.size());
    result["lines"] = std::move(lines);
    return result.dump();
}

/**
 * @brief Emit * or ** regex fragment; advances i past consumed chars.
 *
 * Both single-star and double-star map to .* (this tool matches against
 * workspace-relative paths, so crossing path separators is expected).
 * The key difference: ** also consumes the second star and any trailing
 * slash so the pattern does not generate ".*.*\/" (which requires a
 * literal slash in the subject and would miss root-level files).
 *
 * @param pat     Pattern string.
 * @param i       Current position (the first star); advanced in-place.
 * @param out     Regex output buffer.
 * @dg_internal
 * @version 2.10.0
 */
void emit_glob_star(const std::string& pat, size_t& i, std::string& out) {
    out += ".*";
    bool dbl = (i + 1 < pat.size()) && (pat[i + 1] == '*');
    if (!dbl) { return; }
    ++i;
    if (i + 1 < pat.size() && pat[i + 1] == '/') { ++i; }
}

/**
 * @brief Check if a path matches a glob pattern.
 *
 * Supports ** (cross-directory), * (single segment), and ? (single char).
 * Issue #13 (v2.1.4): regex errors are caught and treated as non-match.
 * gh#126 (v2.10.0): ** and * are now distinguished (see emit_glob_star);
 * the subject is a workspace-relative path, not a bare filename.
 *
 * @param filename Workspace-relative path to test.
 * @param pattern  Glob pattern (already brace-expanded).
 * @return true if filename matches.
 * @dg_internal
 * @version 2.10.0
 */
bool glob_match(const std::string& filename,
                const std::string& pattern) {
    std::string regex_str;
    regex_str.reserve(pattern.size() * 2);

    for (size_t i = 0; i < pattern.size(); ++i) {
        char ch = pattern[i];
        if (ch == '*') {
            emit_glob_star(pattern, i, regex_str);
        } else if (ch == '?') {
            regex_str += '.';
        } else if (ch == '.') {
            regex_str += "\\.";
        } else {
            regex_str += ch;
        }
    }

    try {
        std::regex re(regex_str, std::regex::icase);
        return std::regex_match(filename, re);
    } catch (const std::regex_error& e) {
        logger->warn(
            "glob_match: malformed pattern '{}' → {} — treated as "
            "non-match", pattern, e.what());
        return false;
    }
}

/**
 * @brief Expand brace alternatives in a glob pattern.
 *
 * Issue #13 (v2.1.4): pre-2.1.4 a pattern like `**\/*.{c,h}` threw
 * `std::regex_error` ("Invalid range in '{}'") which propagated as an
 * unhandled exception that killed the entire MCP session. Models
 * trained on shell glob conventions emit this syntax routinely; the
 * graceful fix is to support it.
 *
 * `{a,b,c}` expands to three patterns. Multiple brace groups multiply:
 *   `**\/*.{c,h}.{old,new}`  →  4 patterns (c.old, c.new, h.old, h.new).
 *
 * Nested braces are NOT supported (rare in glob usage); a malformed
 * input produces a single pass-through pattern with the literal braces.
 *
 * @param pattern Source pattern.
 * @return Vector of brace-expanded patterns. At minimum a single entry
 *         (the input verbatim) when no braces are present.
 * @dg_internal
 * @version 2.1.4
 */
/**
 * @brief Split the body of a `{a,b,c}` group into alternatives.
 * @param body Text between the braces (no `{` or `}`).
 * @return Vector of alternative strings. At least one entry.
 * @dg_internal
 * @version 2.1.4
 */
std::vector<std::string> split_brace_alternatives(
    const std::string& body) {
    std::vector<std::string> out;
    std::string current;
    for (char c : body) {
        if (c == ',') {
            out.push_back(std::move(current));
            current.clear();
        } else {
            current += c;
        }
    }
    out.push_back(std::move(current));
    return out;
}

/**
 * @brief Multiply existing patterns by alternatives.
 * @dg_internal
 * @version 2.1.4
 */
std::vector<std::string> multiply_alternatives(
    const std::vector<std::string>& bases,
    const std::vector<std::string>& alternatives) {
    std::vector<std::string> next;
    next.reserve(bases.size() * alternatives.size());
    for (const auto& base : bases) {
        for (const auto& alt : alternatives) {
            next.push_back(base + alt);
        }
    }
    return next;
}

/**
 * @brief Expand `{a,b,c}` brace alternatives in a glob pattern.
 *
 * Issue #13 (v2.1.4). Returns at minimum a single entry equal to the
 * input when no braces are present. Multiple groups multiply
 * (`*.{c,h}.{old,new}` → 4 patterns). Nested braces are not supported;
 * unbalanced braces pass through verbatim.
 *
 * @dg_internal
 * @version 2.1.4
 */
std::vector<std::string> expand_braces(const std::string& pattern) {
    std::vector<std::string> out{""};
    size_t i = 0;
    while (i < pattern.size()) {
        char c = pattern[i];
        auto close = (c == '{')
            ? pattern.find('}', i + 1)
            : std::string::npos;
        bool is_group = (c == '{') && (close != std::string::npos);
        if (!is_group) {
            for (auto& s : out) { s += c; }
            ++i;
            continue;
        }
        auto body = pattern.substr(i + 1, close - i - 1);
        out = multiply_alternatives(
            out, split_brace_alternatives(body));
        i = close + 1;
    }
    return out;
}

/**
 * @brief Enforce read-before-write policy on existing files.
 *
 * A file that does not yet exist is creatable without a prior read;
 * an existing one must have been read this session.
 *
 * @param tracker File access tracker.
 * @param path Canonical path string.
 * @return An empty string when the write may proceed; otherwise a
 *         structured `read_before_write` error naming the file, logged
 *         at warning level.
 * @req REQ-MCP-021
 * @version 1.8.5
 */
std::string check_read_before_write(
    const FileAccessTracker& tracker,
    const std::string& path) {
    if (fs::exists(path) && !tracker.was_read(path)) {
        logger->warn("Read-before-write violation: {}", path);
        return make_error("read_before_write",
            "File must be read before writing: " + path);
    }
    return "";
}

/**
 * @brief Count occurrences of a substring.
 * @param content Text to search.
 * @param needle Substring to count.
 * @return Occurrence count.
 * @dg_internal
 * @version 1.8.6
 */
int count_occurrences(const std::string& content,
                      const std::string& needle) {
    int count = 0;
    size_t pos = 0;
    while ((pos = content.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

/**
 * @brief Replace old_str with new_str in content.
 * @param content File content.
 * @param old_str String to find.
 * @param new_str Replacement string.
 * @param replace_all Replace all occurrences vs single.
 * @param error_type Output: "not_found" or "multiple_matches" on failure.
 * @return Modified content, or nullopt on error.
 * @dg_internal
 * @version 1.8.5
 */
std::optional<std::string>
apply_str_replace(const std::string& content, const std::string& old_str, const std::string& new_str, bool replace_all, std::string& error_type) {

    int occurrences = count_occurrences(content, old_str);
    if (occurrences == 0) {
        error_type = "not_found";
        return std::nullopt;
    }
    if (!replace_all && occurrences > 1) {
        error_type = "multiple_matches";
        return std::nullopt;
    }

    std::string result = content;
    auto pos = result.find(old_str);
    while (pos != std::string::npos) {
        result.replace(pos, old_str.size(), new_str);
        if (!replace_all) { break; }
        pos = result.find(old_str, pos + new_str.size());
    }
    return result;
}

/**
 * @brief Insert text before a given line number.
 * @param content File content.
 * @param line_num 1-based line number.
 * @param new_str Text to insert.
 * @return Modified content.
 * @dg_internal
 * @version 2.1.4
 */
std::string apply_insert(const std::string& content,
                         int line_num,
                         const std::string& new_str) {
    // (#13/#15 v2.1.4: function body unchanged; doxygen-guard parser
    // re-evaluates the position after surrounding helpers were added.)
    std::istringstream stream(content);
    std::ostringstream out;
    std::string line;
    int current = 0;

    while (std::getline(stream, line)) {
        ++current;
        if (current == line_num) {
            out << new_str << '\n';
        }
        out << line << '\n';
    }

    if (line_num > current) {
        out << new_str << '\n';
    }
    return out.str();
}


/**
 * @brief Test whether a filename matches any of the brace-expanded patterns.
 * @param filename Filename to test.
 * @param patterns Patterns (post brace expansion).
 * @return true if any pattern matches.
 * @dg_internal
 * @version 2.1.4
 */
bool any_glob_match(const std::string& filename,
                    const std::vector<std::string>& patterns) {
    for (const auto& p : patterns) {
        if (glob_match(filename, p)) { return true; }
    }
    return false;
}

/**
 * @brief Collect files matching glob, honoring IgnoreMatcher and
 *        brace-expanded patterns.
 *
 * Issue #13/#15 (v2.1.4): brace expansion + path-relative gitignore
 * filtering. Pre-2.1.4 brace expansion raised regex_error and killed
 * the session; build artifacts leaked into results.
 *
 * @param root Starting directory for traversal.
 * @param pattern Glob pattern (may contain `{a,b,c}`).
 * @param max_results Maximum number of results.
 * @param ignore Optional ignore matcher (nullptr disables filtering).
 * @return Vector of matching absolute path strings (deduped).
 * @dg_internal
 * @version 2.1.4
 */
/**
 * @brief Disposition for a single recursive-iterator entry.
 *
 * Encapsulates the decision tree for "skip me / prune below / take me"
 * so collect_glob_matches can stay flat and within the complexity gate.
 *
 * @dg_internal
 * @version 2.1.4
 */
enum class EntryAction {
    kSkip,         ///< Not a regular file or doesn't match — skip
    kSkipPrune,    ///< Skip AND don't descend into this directory
    kTake          ///< Add to results
};

/**
 * @brief Classify a directory_iterator entry against ignore rules and
 *        the brace-expanded glob patterns.
 *
 * @param entry Recursive-iterator entry.
 * @param root Workspace root (for path relativization).
 * @param patterns Brace-expanded glob patterns.
 * @param ignore Optional ignore matcher (nullptr disables filtering).
 * @return kSkipPrune for an ignored or hardcoded-skip directory, so the
 *         walk never descends into it; kSkip for an ignored file or a
 *         non-match; kTake for a regular file matching any pattern.
 *         Matching is done on the path RELATIVE to the root, which is
 *         what makes path-anchored ignore rules work.
 * @req REQ-MCP-022
 * @version 2.10.0
 */
EntryAction classify_glob_entry(
    const fs::directory_entry& entry,
    const fs::path& root,
    const std::vector<std::string>& patterns,
    const IgnoreMatcher* ignore) {
    bool is_dir = entry.is_directory();
    EntryAction result = EntryAction::kSkip;
    bool hardcoded_skip = is_dir
        && should_skip_dir(entry.path().filename().string());
    auto rel = fs::relative(entry.path(), root).generic_string();
    bool ignore_hit = !hardcoded_skip
        && ignore != nullptr
        && !rel.empty()
        && ignore->is_ignored(rel, is_dir);
    if (hardcoded_skip || (ignore_hit && is_dir)) {
        result = EntryAction::kSkipPrune;
    } else if (ignore_hit) {
        result = EntryAction::kSkip;
    } else if (entry.is_regular_file()
               && any_glob_match(rel, patterns)) {
        result = EntryAction::kTake;
    }
    return result;
}

/**
 * @brief Collect files matching a glob, honoring IgnoreMatcher and
 *        brace-expanded patterns.
 *
 * Issues #13/#15 (v2.1.4). Pre-2.1.4 brace expansion threw
 * regex_error and killed the session; build artifacts also leaked
 * into results. This single function now handles both fixes via the
 * expand_braces + classify_glob_entry helpers.
 *
 * @param root Starting directory for traversal.
 * @param pattern Glob pattern (may contain `{a,b,c}`).
 * @param max_results Maximum number of results.
 * @param ignore Optional ignore matcher (nullptr disables filtering).
 * @param[in,out] budget Entry budget; the walk stops when it is spent
 *                and the budget records that it was (gh#161).
 * @return Absolute paths of matching regular files, capped at
 *         `max_results`, with ignored files omitted and ignored
 *         directories never descended into. `**` matches files at the
 *         root as well as in subdirectories (gh#126).
 * @req REQ-MCP-022
 * @req REQ-MCP-021
 * @version 2.13.0
 */
std::vector<std::string> collect_glob_matches(
    const fs::path& root,
    const std::string& pattern,
    int max_results,
    const IgnoreMatcher* ignore,
    WalkBudget& budget) {

    auto patterns = expand_braces(pattern);
    std::vector<std::string> matches;
    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied);

    for (auto& entry : it) {
        if (static_cast<int>(matches.size()) >= max_results) {
            break;
        }
        auto action = classify_glob_entry(entry, root, patterns,
                                          ignore);
        if (action == EntryAction::kSkipPrune) {
            it.disable_recursion_pending();
        } else if (action == EntryAction::kTake) {
            matches.push_back(entry.path().string());
        }
        if (walk_budget_spent(budget)) { break; }
    }
    return matches;
}

/**
 * @brief Search a single file for regex matches.
 *
 * Bounded by a shared total-match limit rather than a per-file one, so
 * a single huge file cannot crowd out the rest of the search.
 *
 * @param path File path.
 * @param re Compiled regex.
 * @param matches Output vector for match results, appended to in place.
 * @param limit Maximum total matches across the whole grep.
 * @req REQ-MCP-022
 * @version 1.8.5
 */
void grep_file(const fs::path& path,
               const std::regex& re,
               std::vector<json>& matches,
               int limit) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return;
    }

    std::string line;
    int line_num = 0;
    while (std::getline(in, line)) {
        ++line_num;
        if (static_cast<int>(matches.size()) >= limit) {
            return;
        }
        if (!std::regex_search(line, re)) {
            continue;
        }
        json m;
        m["path"] = path.string();
        m["line"] = line_num;
        m["content"] = line;
        matches.push_back(std::move(m));
    }
}

/**
 * @brief Convert a directory entry to a JSON object.
 * @param entry Directory entry.
 * @return JSON with name, type, and size fields.
 * @dg_internal
 * @version 1.8.5
 */
json entry_to_json(const fs::directory_entry& entry) {
    json j;
    j["name"] = entry.path().filename().string();

    if (entry.is_directory()) {
        j["type"] = "directory";
        j["size"] = 0;
    } else {
        j["type"] = "file";
        j["size"] = entry.is_regular_file()
            ? static_cast<int64_t>(entry.file_size())
            : 0;
    }
    return j;
}

/**
 * @brief Collect directory entries, optionally recursive.
 * @param dir Directory to list.
 * @param recursive Whether to recurse into subdirectories (defaults to
 *                  false at the tool boundary, gh#116).
 * @param max_depth Maximum recursion depth (tool default 3, gh#116);
 *                  deeper entries are pruned rather than listed.
 * @return One JSON object per listed entry carrying name, type and
 *         size.
 * @req REQ-MCP-021
 * @version 1.8.5
 */
std::vector<json> collect_entries(const fs::path& dir,
                                  bool recursive,
                                  int max_depth) {
    std::vector<json> entries;

    if (!recursive) {
        for (auto& entry : fs::directory_iterator(dir)) {
            entries.push_back(entry_to_json(entry));
        }
        return entries;
    }

    auto it = fs::recursive_directory_iterator(
        dir, fs::directory_options::skip_permission_denied);
    for (auto& entry : it) {
        if (it.depth() > max_depth) {
            it.disable_recursion_pending();
            continue;
        }
        entries.push_back(entry_to_json(entry));
    }
    return entries;
}

/**
 * @brief Apply string replacement mode of edit_file.
 * @param args Parsed JSON arguments.
 * @param content Current file content.
 * @param out Modified content (output).
 * @return An empty string on success; otherwise a structured
 *         `not_found` error when old_string is absent, or
 *         `multiple_matches` (with the use-replace_all hint) when it is
 *         ambiguous — an ambiguous edit is refused, never guessed.
 * @req REQ-MCP-021
 * @version 1.8.6
 */
std::string do_str_replace(const json& args,
                           const std::string& content,
                           std::string& out) {
    auto old_str = args.at("old_string").get<std::string>();
    auto new_str = args.at("new_string").get<std::string>();
    bool replace_all = args.value("replace_all", false);

    std::string error_type;
    auto result = apply_str_replace(
        content, old_str, new_str, replace_all, error_type);
    if (!result.has_value()) {
        auto msg = (error_type == "multiple_matches")
            ? "old_string found multiple times — use replace_all"
            : "old_string not found in file";
        return make_error(error_type, msg);
    }
    out = result.value();
    return "";
}

/**
 * @brief Apply line insertion mode of edit_file.
 * @param args Parsed JSON arguments.
 * @param content Current file content.
 * @param out Modified content (output).
 * @return Empty string (always succeeds).
 * @dg_internal
 * @version 1.8.5
 */
std::string do_insert(const json& args,
                      const std::string& content,
                      std::string& out) {
    auto line_num = args.at("insert_line").get<int>();
    auto new_str = args.at("new_string").get<std::string>();
    out = apply_insert(content, line_num, new_str);
    return "";
}

/**
 * @brief Apply an edit operation and return result JSON or error.
 * @param args Parsed arguments — `old_string` selects replace mode,
 *             `insert_line` selects insert mode.
 * @param resolved Resolved (root-confined) file path.
 * @param path_str Path as string for logging.
 * @return A success JSON object naming the edited path when the edit
 *         applied and was written back; otherwise the mode's structured
 *         error, or `invalid_args` when neither mode was selected. The
 *         file is left untouched on every error path.
 * @req REQ-MCP-021
 * @version 1.8.5
 */
std::string apply_edit(const json& args,
                       const std::filesystem::path& resolved,
                       const std::string& path_str) {
    auto content = read_file_contents(resolved);
    std::string edited;
    std::string err;

    if (args.contains("old_string")) {
        err = do_str_replace(args, content, edited);
    } else if (args.contains("insert_line")) {
        err = do_insert(args, content, edited);
    } else {
        return make_error("invalid_args",
            "edit_file requires old_string or insert_line");
    }

    if (!err.empty()) {
        return err;
    }

    write_file_contents(resolved, edited);
    logger->info("Edited file: {}", path_str);

    json j;
    j["path"] = path_str;
    j["message"] = "Edit applied successfully";
    return j.dump();
}

} // anonymous namespace

// ── ReadFileTool ─────────────────────────────────────────

/**
 * @brief Tool for reading file contents with line numbering.
 * @dg_internal
 * @version 1.8.5
 */
class ReadFileTool : public ToolBase {
public:
    /**
     * @brief Construct from server reference and data directory.
     * @param server Owning filesystem server.
     * @param data_dir Path to bundled data directory.
     * @dg_internal
     * @version 1.8.5
     */
    ReadFileTool(FilesystemServer& server,
                 const std::string& data_dir)
        : ToolBase(load_tool_definition(
              "read_file", "filesystem",
              data_dir + "/tools")),
          server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool only inspects the workspace.
     * @req REQ-MCP-011
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Read a file and return numbered lines as JSON.
     * @param args_json JSON with "path" key.
     * @return ServerResponse with file content or error.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

    /**
     * @brief Anchor key for context deduplication.
     *
     * A non-empty key is what makes MCPServerBase auto-inject the
     * context_anchor directive for this tool.
     *
     * @param args_json JSON with "path" key.
     * @return "file:{path}" anchor key.
     * @req REQ-MCP-001
     * @req REQ-MCP-021
     * @version 1.8.5
     */
    std::string anchor_key(
        const std::string& args_json) const override {
        auto args = json::parse(args_json);
        return "file:" + args.at("path").get<std::string>();
    }

private:
    FilesystemServer& server_;
};

/**
 * @brief Execute read_file: resolve, size-check, read, hash, track.
 * @param args_json JSON arguments.
 * @return ServerResponse with content or error.
 * @dg_internal
 * @version 1.8.5
 */
/**
 * @brief Run pre-read gates (existence, ignore, size cap).
 *
 * Returns a non-empty error JSON when any gate denies. Path-relative
 * ignore matching uses the server's IgnoreMatcher (#15, v2.1.4).
 *
 * @param server Owning server (source of the root, ignore rules and
 *               size limit).
 * @param resolved Already root-confined path.
 * @param path_str Resolved path as a string, for messages.
 * @return An empty string when the read may proceed; otherwise a
 *         structured error — `not_found` (whose message names
 *         list_directory, gh#124), `is_directory` for a directory
 *         argument (gh#116), `ignored` for a path excluded by
 *         .gitignore/.explorerignore, or `size_exceeded` naming both
 *         the size and the limit so the model pivots to offset/limit.
 * @req REQ-MCP-021
 * @req REQ-MCP-022
 * @version 2.10.0
 */
std::string check_read_gates(FilesystemServer& server,
                             const fs::path& resolved,
                             const std::string& path_str) {
    std::string err;
    if (!fs::exists(resolved)) {
        err = make_error("not_found",
            "File not found: " + path_str
            + ". Use list_directory(\".\") to see available files"
              " in the working directory, or verify the path is"
              " relative to the configured root.");
    } else if (fs::is_directory(resolved)) {
        err = make_error("is_directory",
            "Path is a directory, not a file: " + path_str);
    } else {
        auto rel = fs::relative(resolved, server.root_dir())
                     .generic_string();
        bool ignored = !rel.empty()
            && server.ignore().is_ignored(rel, /*is_dir=*/false);
        int size = static_cast<int>(fs::file_size(resolved));
        int limit = server.max_read_bytes();
        if (ignored) {
            err = make_error("ignored",
                "Path '" + rel + "' is excluded by .gitignore or "
                ".explorerignore.");
        } else if (limit > 0 && size > limit) {
            err = make_error("size_exceeded",
                "File " + path_str + " is " +
                std::to_string(size) + " bytes (limit: " +
                std::to_string(limit) + ")");
        }
    }
    return err;
}

/**
 * @brief Execute read_file: resolve, gate, read, hash, track.
 *
 * Issue #15 (v2.1.4): pre-read gating moved into check_read_gates so
 * the ignore-matcher refusal lives next to existence + size checks.
 *
 * A successful read records the content hash in the FileAccessTracker —
 * which is why read_file must always execute and opts out of duplicate
 * detection.
 *
 * @param args_json JSON with a "path" key.
 * @return A ServerResponse with no directives whose result is either
 *         the numbered-lines JSON or the first failing gate's
 *         structured error.
 * @req REQ-MCP-021
 * @version 2.1.4
 */
ServerResponse ReadFileTool::execute(const std::string& args_json) {
    auto args = json::parse(args_json);
    auto requested = args.at("path").get<std::string>();
    auto resolved = server_.resolve_path(requested);
    auto path_str = resolved.string();

    auto err = check_read_gates(server_, resolved, path_str);
    if (!err.empty()) { return {err, {}}; }

    auto content = read_file_contents(resolved);
    server_.tracker().record_read(path_str,
                                  hash_content(content));
    auto size = static_cast<int>(fs::file_size(resolved));
    logger->info("Read file: {} ({} bytes)", path_str, size);
    return {build_read_result(path_str, content), {}};
}

// ── WriteFileTool ────────────────────────────────────────

/**
 * @brief Tool for writing file contents with read-before-write.
 * @dg_internal
 * @version 1.8.5
 */
class WriteFileTool : public ToolBase {
public:
    /**
     * @brief Construct from server reference and data directory.
     * @param server Owning filesystem server.
     * @param data_dir Path to bundled data directory.
     * @dg_internal
     * @version 1.8.5
     */
    WriteFileTool(FilesystemServer& server,
                  const std::string& data_dir)
        : ToolBase(load_tool_definition(
              "write_file", "filesystem",
              data_dir + "/tools")),
          server_(server) {}

    /**
     * @brief Write content to a file after read-before-write check.
     * @param args_json JSON with "path" and "content" keys.
     * @return ServerResponse with result or error.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    FilesystemServer& server_;
};

/**
 * @brief Execute write_file: resolve, enforce policy, write.
 *
 * The path is resolved against the root first (so a traversal attempt
 * never reaches the write), then the read-before-write gate runs.
 *
 * @param args_json JSON with "path" and "content" keys.
 * @return A ServerResponse with no directives whose result is either a
 *         success JSON naming the path and bytes written, or the
 *         structured `read_before_write` error — in which case nothing
 *         is written.
 * @req REQ-MCP-021
 * @version 1.8.5
 */
ServerResponse WriteFileTool::execute(
    const std::string& args_json) {

    auto args = json::parse(args_json);
    auto requested = args.at("path").get<std::string>();
    auto content = args.at("content").get<std::string>();
    auto resolved = server_.resolve_path(requested);
    auto path_str = resolved.string();

    auto violation = check_read_before_write(
        server_.tracker(), path_str);
    if (!violation.empty()) {
        return {violation, {}};
    }

    write_file_contents(resolved, content);
    logger->info("Wrote file: {} ({} bytes)",
                 path_str, content.size());

    json result;
    result["path"] = path_str;
    result["bytes_written"] = content.size();
    result["message"] = "File written successfully";
    return {result.dump(), {}};
}

// ── EditFileTool ─────────────────────────────────────────

/**
 * @brief Tool for in-place file editing (string replace or insert).
 * @dg_internal
 * @version 1.8.5
 */
class EditFileTool : public ToolBase {
public:
    /**
     * @brief Construct from server reference and data directory.
     * @param server Owning filesystem server.
     * @param data_dir Path to bundled data directory.
     * @dg_internal
     * @version 1.8.5
     */
    EditFileTool(FilesystemServer& server,
                 const std::string& data_dir)
        : ToolBase(load_tool_definition(
              "edit_file", "filesystem",
              data_dir + "/tools")),
          server_(server) {}

    /**
     * @brief Edit a file via string replacement or line insertion.
     * @param args_json JSON with edit parameters.
     * @return ServerResponse with result or error.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    FilesystemServer& server_;
};

/**
 * @brief Execute edit_file: read, apply edit, write back.
 * @param args_json JSON arguments.
 * @return ServerResponse with result.
 * @dg_internal
 * @version 1.8.5
 */
ServerResponse EditFileTool::execute(const std::string& args_json) {
    auto args = json::parse(args_json);
    auto requested = args.at("path").get<std::string>();
    auto resolved = server_.resolve_path(requested);
    auto path_str = resolved.string();

    auto violation = check_read_before_write(
        server_.tracker(), path_str);
    if (!violation.empty()) {
        return {violation, {}};
    }

    auto result = apply_edit(args, resolved, path_str);
    ServerResponse resp;
    resp.result = result;
    return resp;
}

// ── GlobTool ─────────────────────────────────────────────

/**
 * @brief Tool for recursive file pattern matching.
 * @dg_internal
 * @version 1.8.5
 */
class GlobTool : public ToolBase {
public:
    /**
     * @brief Construct with server reference and data directory.
     * @param server Owning FilesystemServer (for root_dir).
     * @param data_dir Path to bundled data directory.
     * @dg_internal
     * @version 2.0.4
     */
    GlobTool(FilesystemServer& server, const std::string& data_dir)
        : ToolBase(load_tool_definition(
              "glob", "filesystem",
              data_dir + "/tools")),
          server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool only inspects the workspace.
     * @req REQ-MCP-011
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Find files matching a glob pattern.
     * @param args_json JSON with "pattern" key.
     * @return ServerResponse with matched file paths.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    FilesystemServer& server_; ///< Owning server (for root_dir)
};

/**
 * @brief Execute glob: find files matching pattern.
 *
 * Issues #13/#15 (v2.1.4): brace expansion in the pattern, ignore-
 * matcher filtering on results.
 *
 * @param args_json JSON arguments.
 * @return ServerResponse with matched paths.
 * @dg_internal
 * @version 2.13.0
 */
ServerResponse GlobTool::execute(const std::string& args_json) {
    auto args = json::parse(args_json);
    auto pattern = args.at("pattern").get<std::string>();
    constexpr int MAX_GLOB_RESULTS = 500;

    // Issue #15 (v2.1.4): pass server's IgnoreMatcher so build/, vendor/,
    // doxygen/, and anything else listed in .gitignore + .explorerignore
    // is filtered out. Pre-2.1.4 only the hardcoded SKIP_DIRS were honored.
    // Issue #13 (v2.1.4): brace expansion handled inside.
    WalkBudget budget{server_.config().max_walk_entries, 0, false};
    auto matches = collect_glob_matches(
        server_.root_dir(), pattern, MAX_GLOB_RESULTS,
        &server_.ignore(), budget);

    logger->info("Glob '{}': {} matches, {} entries walked "
                 "(after ignore filtering)",
                 pattern, matches.size(), budget.visited);
    json result = matches;
    note_truncated_walk(result, budget, "Glob", pattern);
    return {result.dump(), {}};
}

// ── GrepTool ─────────────────────────────────────────────

/**
 * @brief Tool for regex content search across files.
 * @dg_internal
 * @version 1.8.5
 */
class GrepTool : public ToolBase {
public:
    /**
     * @brief Construct from data directory.
     * @param data_dir Path to bundled data directory.
     * @dg_internal
     * @version 2.1.4
     */
    GrepTool(FilesystemServer& server, const std::string& data_dir)
        : ToolBase(load_tool_definition(
              "grep", "filesystem",
              /* tools dir: */ data_dir + "/tools")),
          server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool only inspects the workspace.
     * @req REQ-MCP-011
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Search files for regex pattern matches.
     * @param args_json JSON with "pattern" and optional "glob" keys.
     * @return ServerResponse with match results.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    FilesystemServer& server_; ///< Owning server (for root_dir)
};

/**
 * @brief Compile a regex or return a structured tool error.
 *
 * @param pattern Regex source.
 * @param[out] err Set to a non-empty JSON error string on failure.
 * @return The compiled regex on success; on failure a never-matching
 *         regex with `err` set to a structured `invalid_regex` error —
 *         a bad pattern from the model is a tool error, never a throw
 *         through the dispatch path.
 * @req REQ-MCP-021
 * @version 2.1.4
 */
std::regex compile_grep_or_error(const std::string& pattern,
                                 std::string& err) {
    try {
        return std::regex(pattern);
    } catch (const std::regex_error& e) {
        err = make_error("invalid_regex", e.what());
        return std::regex("(?!)");
    }
}

/**
 * @brief Execute grep: brace-expand the file glob, compile the
 *        content regex (error-safe), iterate the tree applying the
 *        same classify_glob_entry filter glob uses.
 *
 * Issues #13/#15 (v2.1.4).
 *
 * @dg_internal
 * @version 2.1.4
 */
/**
 * @brief Walk the tree applying the glob/ignore filter and grep files.
 * @param root Search root.
 * @param file_patterns Brace-expanded glob patterns.
 * @param re Compiled content regex.
 * @param ignore Ignore matcher.
 * @param[in,out] budget Entry budget; the walk stops when it is spent
 *                and the budget records that it was (gh#161). grep
 *                needs this MORE than glob does — it also opens and
 *                reads every matching file.
 * @return Up to MAX_GREP_RESULTS match objects. Uses exactly the same
 *         classify_glob_entry filter glob does, so grep and glob honour
 *         .gitignore/.explorerignore identically and ignored
 *         directories are pruned rather than walked.
 * @req REQ-MCP-022
 * @version 2.13.0
 */
static std::vector<json> grep_search(
    const fs::path& root, const std::vector<std::string>& file_patterns,
    const std::regex& re, const IgnoreMatcher& ignore,
    WalkBudget& budget) {
    constexpr int MAX_GREP_RESULTS = 100;
    std::vector<json> matches;
    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied);
    for (auto& entry : it) {
        if (static_cast<int>(matches.size()) >= MAX_GREP_RESULTS) {
            break;
        }
        auto action = classify_glob_entry(entry, root, file_patterns,
                                          &ignore);
        if (action == EntryAction::kSkipPrune) {
            it.disable_recursion_pending();
        } else if (action == EntryAction::kTake) {
            grep_file(entry.path(), re, matches, MAX_GREP_RESULTS);
        }
        if (walk_budget_spent(budget)) { break; }
    }
    return matches;
}

/**
 * @brief Execute grep: compile regex, walk the tree, collect matches.
 * @param args_json JSON with a "pattern" key and an optional "glob"
 *                  (defaulting to "*").
 * @return A ServerResponse with no directives whose result is the JSON
 *         array of matches, or the structured `invalid_regex` error
 *         when the pattern would not compile.
 * @req REQ-MCP-022
 * @req REQ-MCP-021
 * @version 2.13.0
 */
ServerResponse GrepTool::execute(const std::string& args_json) {
    auto args = json::parse(args_json);
    auto pattern = args.at("pattern").get<std::string>();
    auto file_glob = args.value("glob", std::string("*"));

    std::string err;
    auto re = compile_grep_or_error(pattern, err);
    if (!err.empty()) { return {err, {}}; }

    auto file_patterns = expand_braces(file_glob);
    WalkBudget budget{server_.config().max_walk_entries, 0, false};
    auto matches = grep_search(server_.root_dir(), file_patterns, re,
                               server_.ignore(), budget);

    logger->info("Grep '{}': {} matches, {} entries walked "
                 "(after ignore filtering)",
                 pattern, matches.size(), budget.visited);
    json result = matches;
    note_truncated_walk(result, budget, "Grep", pattern);
    return {result.dump(), {}};
}

// ── ListDirectoryTool ────────────────────────────────────

/**
 * @brief Tool for listing directory contents.
 * @dg_internal
 * @version 1.8.5
 */
class ListDirectoryTool : public ToolBase {
public:
    /**
     * @brief Construct from server reference and data directory.
     * @param server Owning filesystem server.
     * @param data_dir Path to bundled data directory.
     * @dg_internal
     * @version 1.8.5
     */
    ListDirectoryTool(FilesystemServer& server,
                      const std::string& data_dir)
        : ToolBase(load_tool_definition(
              "list_directory", "filesystem",
              data_dir + "/tools")),
          server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool only inspects the workspace.
     * @req REQ-MCP-011
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief List directory entries with optional recursion.
     * @param args_json JSON with "path" and optional depth params.
     * @return ServerResponse with directory listing.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    FilesystemServer& server_;
};

/**
 * @brief Execute list_directory: list entries in a directory.
 *
 * The optional parameters carry documented defaults (gh#116): `path`
 * defaults to the project root, `recursive` to false, `max_depth` to 3.
 *
 * @param args_json JSON with optional "path", "recursive" and
 *                  "max_depth" keys — all three may be omitted.
 * @return A ServerResponse with no directives whose result is the JSON
 *         array of entries, or a structured `not_directory` error when
 *         the resolved path is not a directory.
 * @req REQ-MCP-021
 * @version 2.9.13
 */
ServerResponse ListDirectoryTool::execute(
    const std::string& args_json) {

    auto args = json::parse(args_json);
    auto requested = args.value("path", std::string("."));
    auto recursive = args.value("recursive", false);
    auto max_depth = args.value("max_depth", 3);

    auto resolved = server_.resolve_path(requested);
    if (!fs::is_directory(resolved)) {
        return {make_error("not_directory",
            "Not a directory: " + resolved.string()), {}};
    }

    auto entries = collect_entries(
        resolved, recursive, max_depth);

    logger->info("Listed {}: {} entries",
                 resolved.string(), entries.size());
    json result = entries;
    return {result.dump(), {}};
}

// ── FilesystemServer ─────────────────────────────────────

/**
 * @brief Compute max read bytes from config and model context.
 * @param config Filesystem configuration.
 * @param model_context_bytes Model context window in bytes.
 * @return The explicit config `max_read_bytes` when set; else a
 *         percentage of the model context; else a 32KB default — the
 *         gate that turns an oversized read into a `size_exceeded`
 *         error instead of a blown context budget.
 * @req REQ-MCP-021
 * @version 2.0.4
 */
static int compute_max_read_bytes(const FilesystemConfig& config,
                                  int model_context_bytes) {
    if (config.max_read_bytes.has_value()) {
        return config.max_read_bytes.value();
    }
    if (model_context_bytes <= 0) {
        // Safe default: 32KB prevents a single file from blowing typical
        // context budgets (16-128K). Large files trigger size_exceeded and
        // the model must use offset/limit or docs.* tools instead.
        return 32 * 1024;
    }
    return static_cast<int>(
        model_context_bytes * config.max_read_context_pct);
}

/**
 * @brief Construct filesystem server, create and register all tools.
 *
 * Issue #15 (v2.1.4): also loads .gitignore + .explorerignore via the
 * IgnoreMatcher member so glob/grep/read filter ignored paths.
 *
 * @param root_dir Project root directory — canonicalised here, and the
 *                 boundary every tool's paths are confined to.
 * @param config Filesystem configuration.
 * @param data_dir Path to bundled data directory.
 * @param model_context_bytes Model context window in bytes.
 * @req REQ-MCP-001
 * @req REQ-MCP-021
 * @req REQ-MCP-022
 * @version 2.3.7
 */
FilesystemServer::FilesystemServer(
    const fs::path& root_dir,
    const FilesystemConfig& config,
    const std::string& data_dir,
    int model_context_bytes)
    : MCPServerBase("filesystem"),
      root_dir_(fs::weakly_canonical(root_dir)),
      config_(config),
      max_read_bytes_(compute_max_read_bytes(
          config, model_context_bytes)) {

    create_fs_tools(data_dir);
    register_fs_tools();

    // Issue #15 (v2.1.4): load .gitignore + .explorerignore so glob,
    // grep, and read_file can filter out build artifacts and vendor
    // blobs that pre-2.1.4 leaked into results.
    ignore_.load(root_dir_);

    logger->info("FilesystemServer initialized: root={}, "
                 "max_read_bytes={}, ignore_rules={}",
                 root_dir_.string(),
                 max_read_bytes_,
                 ignore_.rule_count());
}

/**
 * @brief Construct the six filesystem tool instances (ctor step 1).
 *
 * read_file, write_file, edit_file, glob, grep and list_directory —
 * the server's whole surface, each built from its bundled JSON
 * descriptor.
 *
 * @param data_dir Directory holding tool JSON definitions.
 * @req REQ-MCP-021
 * @version 2.3.7
 */
void FilesystemServer::create_fs_tools(const std::string& data_dir) {
    read_file_ = std::make_unique<ReadFileTool>(*this, data_dir);
    write_file_ = std::make_unique<WriteFileTool>(*this, data_dir);
    edit_file_ = std::make_unique<EditFileTool>(*this, data_dir);
    glob_ = std::make_unique<GlobTool>(*this, data_dir);
    grep_ = std::make_unique<GrepTool>(*this, data_dir);
    list_dir_ = std::make_unique<ListDirectoryTool>(*this, data_dir);
}

/**
 * @brief Register the six filesystem tools (ctor step 2).
 *
 * Registration is all it takes — dispatch, envelope shape and the
 * read_file context anchor come from MCPServerBase.
 *
 * @req REQ-MCP-001
 * @version 2.3.7
 */
void FilesystemServer::register_fs_tools() {
    register_tool(read_file_.get());
    register_tool(write_file_.get());
    register_tool(edit_file_.get());
    register_tool(glob_.get());
    register_tool(grep_.get());
    register_tool(list_dir_.get());
}

/**
 * @brief Destructor (default, unique_ptr cleanup).
 * @dg_internal
 * @version 1.8.5
 */
FilesystemServer::~FilesystemServer() = default;

/**
 * @brief read_file always executes (updates FileAccessTracker).
 *
 * read_file must run even when repeated, because its side effect —
 * recording the read in the tracker — is what unlocks a later write.
 *
 * @param tool_name Tool name to check.
 * @return true for "read_file", false for every other filesystem tool,
 *         which stay duplicate-checked.
 * @req REQ-MCP-015
 * @req REQ-MCP-021
 * @version 1.8.5
 */
bool FilesystemServer::skip_duplicate_check(
    const std::string& tool_name) const {
    return tool_name == "read_file";
}

/**
 * @brief Set working directory by updating root_dir.
 *
 * Issue #15 (v2.1.4): also reloads IgnoreMatcher rules so the new
 * root's .gitignore + .explorerignore take effect immediately.
 *
 * @param path New root directory.
 * @return true when the path is a directory, in which case the server
 *         is re-rooted AND the ignore rules are reloaded from the new
 *         root; false when it is not, leaving the server untouched.
 * @req REQ-MCP-021
 * @req REQ-MCP-022
 * @version 2.1.4
 */
bool FilesystemServer::set_working_dir(const std::string& path) {
    auto canonical = fs::weakly_canonical(path);
    if (!fs::is_directory(canonical)) {
        logger->error("set_working_dir: not a directory: {}",
                      path);
        return false;
    }
    root_dir_ = canonical;
    // Issue #15 (v2.1.4): reload ignore rules for the new root.
    ignore_.load(root_dir_);
    logger->info("Working directory changed to: {} (ignore_rules={})",
                 root_dir_.string(), ignore_.rule_count());
    return true;
}

/**
 * @brief Get the root directory.
 * @return The canonical root every requested path is resolved against
 *         and confined to.
 * @req REQ-MCP-021
 * @version 1.8.5
 */
const fs::path& FilesystemServer::root_dir() const {
    return root_dir_;
}

/**
 * @brief Get the file access tracker.
 * @return Mutable reference to the read-before-write tracker shared by
 *         read_file (which records) and write_file (which enforces).
 * @req REQ-MCP-021
 * @version 1.8.5
 */
FileAccessTracker& FilesystemServer::tracker() {
    return tracker_;
}

/**
 * @brief Get the ignore matcher (#15, v2.1.4).
 * @return Read-only reference to the loaded .gitignore +
 *         .explorerignore rule set that glob, grep and read_file all
 *         consult.
 * @req REQ-MCP-022
 * @version 2.1.4
 */
const IgnoreMatcher& FilesystemServer::ignore() const {
    return ignore_;
}

/**
 * @brief Get the filesystem config.
 * @return Config reference.
 * @dg_internal
 * @version 1.8.5
 */
const FilesystemConfig& FilesystemServer::config() const {
    return config_;
}

/**
 * @brief Get max read bytes for size gate.
 * @return The read size limit in bytes, or 0 for unlimited — a file
 *         over the limit is refused with `size_exceeded`.
 * @req REQ-MCP-021
 * @version 1.8.5
 */
int FilesystemServer::max_read_bytes() const {
    return max_read_bytes_;
}

/**
 * @brief Resolve and validate a path against root directory.
 *
 * Resolves relative paths against root_dir_, canonicalizes, and
 * checks that the result does not escape root. Throws on escape
 * unless allow_outside_root is configured.
 *
 * Containment uses lexically_relative so that "/home/user/project"
 * does not falsely contain "/home/user/projectile" via string-prefix.
 *
 * The single confinement point every filesystem tool goes through, so
 * no tool can traverse outside the project.
 *
 * @param requested User-requested path string (absolute or relative to
 *                  the root).
 * @return The canonical path when it lies under the root — or anywhere,
 *         when allow_outside_root is configured.
 * @throws std::runtime_error when the canonical result leaves the root,
 *         logged as "Path escape blocked".
 * @req REQ-MCP-021
 * @version 2.1.1-rc1
 */
fs::path FilesystemServer::resolve_path(
    const std::string& requested) const {

    fs::path req_path(requested);
    fs::path resolved = req_path.is_absolute()
        ? fs::weakly_canonical(req_path)
        : fs::weakly_canonical(root_dir_ / req_path);

    fs::path rel = resolved.lexically_relative(root_dir_);
    bool under_root = !rel.empty()
        && *rel.begin() != fs::path("..")
        && rel != fs::path("..");

    if (!under_root && !config_.allow_outside_root) {
        logger->error("Path escape blocked: {} (root: {})",
                      resolved.string(), root_dir_.string());
        throw std::runtime_error(
            "Path escapes project root: " + resolved.string());
    }
    return resolved;
}

} // namespace entropic
