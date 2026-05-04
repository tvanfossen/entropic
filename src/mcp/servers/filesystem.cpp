// SPDX-License-Identifier: LGPL-3.0-or-later
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
 * @param path Canonical file path.
 * @param hash Content hash at time of read.
 * @internal
 * @version 1.8.5
 */
void FileAccessTracker::record_read(const std::string& path,
                                    size_t hash) {
    reads_[path] = hash;
    logger->info("Tracked read: {}", path);
}

/**
 * @brief Check if file was read and content is unchanged.
 * @param path Canonical file path.
 * @param current_hash Current content hash.
 * @return true if previously read with matching hash.
 * @internal
 * @version 1.8.5
 */
bool FileAccessTracker::was_read_unchanged(
    const std::string& path,
    size_t current_hash) const {
    auto it = reads_.find(path);
    if (it == reads_.end()) {
        return false;
    }
    return it->second == current_hash;
}

/**
 * @brief Check if a file was ever read in this session.
 * @param path Canonical file path.
 * @return true if previously recorded.
 * @internal
 * @version 1.8.5
 */
bool FileAccessTracker::was_read(const std::string& path) const {
    return reads_.count(path) > 0;
}

// ── File-local helpers ───────────────────────────────────

namespace {

/**
 * @brief Directories to skip during recursive traversal.
 * @internal
 * @version 1.8.5
 */
const std::vector<std::string> SKIP_DIRS = {
    ".git", "node_modules", "__pycache__", ".venv"
};

/**
 * @brief Check if a directory name should be skipped.
 * @param name Directory name to check.
 * @return true if name is in the skip list.
 * @internal
 * @version 1.8.5
 */
bool should_skip_dir(const std::string& name) {
    for (const auto& skip : SKIP_DIRS) {
        if (name == skip) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Read entire file contents into a string.
 * @param path File path.
 * @return File contents.
 * @throws std::runtime_error if file cannot be opened.
 * @internal
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
 * @internal
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
 * @internal
 * @version 1.8.5
 */
size_t hash_content(const std::string& s) {
    return std::hash<std::string>{}(s);
}

/**
 * @brief Build a JSON error response.
 * @param code Error code string.
 * @param message Human-readable message.
 * @return JSON string.
 * @internal
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
 * @return JSON string with path, total lines, and numbered lines.
 * @internal
 * @version 1.8.5
 */
std::string build_read_result(const std::string& path,
                              const std::string& content) {
    json result;
    result["path"] = path;

    std::istringstream stream(content);
    std::string line;
    json lines = json::object();
    int num = 0;

    while (std::getline(stream, line)) {
        ++num;
        lines[std::to_string(num)] = line;
    }

    result["total"] = num;
    result["lines"] = std::move(lines);
    return result.dump();
}

/**
 * @brief Check if a filename matches a glob pattern.
 *
 * Supports '*' (any chars) and '?' (single char) wildcards.
 *
 * Issue #13 (v2.1.4): regex construction is wrapped in try/catch so
 * malformed patterns produce a "no match" result instead of throwing
 * an unhandled exception that kills the session. Brace expansion
 * (handled by expand_braces upstream) means the pattern reaching here
 * is single-alternative and almost always well-formed.
 *
 * @param filename Filename to test.
 * @param pattern Glob pattern (already brace-expanded).
 * @return true if filename matches.
 * @internal
 * @version 2.1.4
 */
bool glob_match(const std::string& filename,
                const std::string& pattern) {
    std::string regex_str;
    regex_str.reserve(pattern.size() * 2);

    for (char ch : pattern) {
        if (ch == '*') {
            regex_str += ".*";
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
 * @internal
 * @version 2.1.4
 */
/**
 * @brief Split the body of a `{a,b,c}` group into alternatives.
 * @param body Text between the braces (no `{` or `}`).
 * @return Vector of alternative strings. At least one entry.
 * @internal
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
 * @internal
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
 * @internal
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
 * @param tracker File access tracker.
 * @param path Canonical path string.
 * @return Error JSON string if violation, empty string if OK.
 * @internal
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
 * @brief Apply string replacement to content.
 * @param content Original content.
 * @param old_str String to find.
 * @param new_str Replacement string.
 * @param replace_all Replace all occurrences if true.
 * @brief Count occurrences of a substring.
 * @param content Text to search.
 * @param needle Substring to count.
 * @return Occurrence count.
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
 * @version 2.1.4
 */
/**
 * @brief Disposition for a single recursive-iterator entry.
 *
 * Encapsulates the decision tree for "skip me / prune below / take me"
 * so collect_glob_matches can stay flat and within the complexity gate.
 *
 * @internal
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
 * @param ignore Optional ignore matcher.
 * @return One of EntryAction values.
 * @internal
 * @version 2.1.4
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
    bool ignore_hit = false;
    if (!hardcoded_skip && ignore != nullptr) {
        auto rel = fs::relative(entry.path(), root)
                     .generic_string();
        ignore_hit = !rel.empty()
            && ignore->is_ignored(rel, is_dir);
    }
    if (hardcoded_skip || (ignore_hit && is_dir)) {
        result = EntryAction::kSkipPrune;
    } else if (ignore_hit) {
        result = EntryAction::kSkip;
    } else if (entry.is_regular_file()
               && any_glob_match(
                      entry.path().filename().string(), patterns)) {
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
 * @internal
 * @version 2.1.4
 */
std::vector<std::string> collect_glob_matches(
    const fs::path& root,
    const std::string& pattern,
    int max_results,
    const IgnoreMatcher* ignore = nullptr) {

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
    }
    return matches;
}

/**
 * @brief Search a single file for regex matches.
 * @param path File path.
 * @param re Compiled regex.
 * @param matches Output vector for match results.
 * @param limit Maximum total matches.
 * @internal
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
 * @internal
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
 * @param recursive Whether to recurse into subdirectories.
 * @param max_depth Maximum recursion depth (0 = immediate only).
 * @return Vector of JSON entry objects.
 * @internal
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
 * @return Empty string on success, error JSON on failure.
 * @internal
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
 * @internal
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
 * @param args Parsed arguments.
 * @param resolved Resolved file path.
 * @param path_str Path as string for logging.
 * @return Result JSON or error string.
 * @internal
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
 * @internal
 * @version 1.8.5
 */
class ReadFileTool : public ToolBase {
public:
    /**
     * @brief Construct from server reference and data directory.
     * @param server Owning filesystem server.
     * @param data_dir Path to bundled data directory.
     * @internal
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
     * @return MCPAccessLevel::READ.
     * @utility
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Read a file and return numbered lines as JSON.
     * @param args_json JSON with "path" key.
     * @return ServerResponse with file content or error.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

    /**
     * @brief Anchor key for context deduplication.
     * @param args_json JSON with "path" key.
     * @return "file:{path}" anchor key.
     * @internal
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
 * @internal
 * @version 1.8.5
 */
/**
 * @brief Run pre-read gates (existence, ignore, size cap).
 *
 * Returns a non-empty error JSON when any gate denies. Path-relative
 * ignore matching uses the server's IgnoreMatcher (#15, v2.1.4).
 *
 * @internal
 * @version 2.1.4
 */
std::string check_read_gates(FilesystemServer& server,
                             const fs::path& resolved,
                             const std::string& path_str) {
    std::string err;
    if (!fs::exists(resolved)) {
        err = make_error("not_found",
            "File not found: " + path_str);
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
 * @internal
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
 * @internal
 * @version 1.8.5
 */
class WriteFileTool : public ToolBase {
public:
    /**
     * @brief Construct from server reference and data directory.
     * @param server Owning filesystem server.
     * @param data_dir Path to bundled data directory.
     * @internal
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
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    FilesystemServer& server_;
};

/**
 * @brief Execute write_file: resolve, enforce policy, write.
 * @param args_json JSON arguments.
 * @return ServerResponse with result.
 * @internal
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
 * @internal
 * @version 1.8.5
 */
class EditFileTool : public ToolBase {
public:
    /**
     * @brief Construct from server reference and data directory.
     * @param server Owning filesystem server.
     * @param data_dir Path to bundled data directory.
     * @internal
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
     * @internal
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
 * @internal
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
 * @internal
 * @version 1.8.5
 */
class GlobTool : public ToolBase {
public:
    /**
     * @brief Construct with server reference and data directory.
     * @param server Owning FilesystemServer (for root_dir).
     * @param data_dir Path to bundled data directory.
     * @internal
     * @version 2.0.4
     */
    GlobTool(FilesystemServer& server, const std::string& data_dir)
        : ToolBase(load_tool_definition(
              "glob", "filesystem",
              data_dir + "/tools")),
          server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ.
     * @utility
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Find files matching a glob pattern.
     * @param args_json JSON with "pattern" key.
     * @return ServerResponse with matched file paths.
     * @internal
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
 * @internal
 * @version 2.1.4
 */
ServerResponse GlobTool::execute(const std::string& args_json) {
    auto args = json::parse(args_json);
    auto pattern = args.at("pattern").get<std::string>();
    constexpr int MAX_GLOB_RESULTS = 500;

    // Issue #15 (v2.1.4): pass server's IgnoreMatcher so build/, vendor/,
    // doxygen/, and anything else listed in .gitignore + .explorerignore
    // is filtered out. Pre-2.1.4 only the hardcoded SKIP_DIRS were honored.
    // Issue #13 (v2.1.4): brace expansion handled inside.
    auto matches = collect_glob_matches(
        server_.root_dir(), pattern, MAX_GLOB_RESULTS,
        &server_.ignore());

    logger->info("Glob '{}': {} matches (after ignore filtering)",
                 pattern, matches.size());
    json result = matches;
    return {result.dump(), {}};
}

// ── GrepTool ─────────────────────────────────────────────

/**
 * @brief Tool for regex content search across files.
 * @internal
 * @version 1.8.5
 */
class GrepTool : public ToolBase {
public:
    /**
     * @brief Construct from data directory.
     * @param data_dir Path to bundled data directory.
     * @internal
     * @version 2.1.4
     */
    GrepTool(FilesystemServer& server, const std::string& data_dir)
        : ToolBase(load_tool_definition(
              "grep", "filesystem",
              /* tools dir: */ data_dir + "/tools")),
          server_(server) {}

    /**
     * @brief Read-only tool — requires READ access.
     * @return MCPAccessLevel::READ.
     * @utility
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief Search files for regex pattern matches.
     * @param args_json JSON with "pattern" and optional "glob" keys.
     * @return ServerResponse with match results.
     * @internal
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
 * @return Compiled regex on success; the error path leaves `err`
 *         populated and returns a never-match regex.
 * @internal
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
 * @internal
 * @version 2.1.4
 */
ServerResponse GrepTool::execute(const std::string& args_json) {
    auto args = json::parse(args_json);
    auto pattern = args.at("pattern").get<std::string>();
    auto file_glob = args.value("glob", std::string("*"));

    std::string err;
    auto re = compile_grep_or_error(pattern, err);
    if (!err.empty()) { return {err, {}}; }

    auto file_patterns = expand_braces(file_glob);
    constexpr int MAX_GREP_RESULTS = 100;
    std::vector<json> matches;
    auto root = server_.root_dir();
    const auto& ignore = server_.ignore();
    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied);

    for (auto& entry : it) {
        if (static_cast<int>(matches.size()) >=
            MAX_GREP_RESULTS) {
            break;
        }
        auto action = classify_glob_entry(entry, root, file_patterns,
                                          &ignore);
        if (action == EntryAction::kSkipPrune) {
            it.disable_recursion_pending();
        } else if (action == EntryAction::kTake) {
            grep_file(entry.path(), re, matches, MAX_GREP_RESULTS);
        }
    }

    logger->info("Grep '{}': {} matches (after ignore filtering)",
                 pattern, matches.size());
    json result = matches;
    return {result.dump(), {}};
}

// ── ListDirectoryTool ────────────────────────────────────

/**
 * @brief Tool for listing directory contents.
 * @internal
 * @version 1.8.5
 */
class ListDirectoryTool : public ToolBase {
public:
    /**
     * @brief Construct from server reference and data directory.
     * @param server Owning filesystem server.
     * @param data_dir Path to bundled data directory.
     * @internal
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
     * @return MCPAccessLevel::READ.
     * @utility
     * @version 1.9.4
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /**
     * @brief List directory entries with optional recursion.
     * @param args_json JSON with "path" and optional depth params.
     * @return ServerResponse with directory listing.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    FilesystemServer& server_;
};

/**
 * @brief Execute list_directory: list entries in a directory.
 * @param args_json JSON arguments.
 * @return ServerResponse with directory listing.
 * @internal
 * @version 1.8.5
 */
ServerResponse ListDirectoryTool::execute(
    const std::string& args_json) {

    auto args = json::parse(args_json);
    auto requested = args.at("path").get<std::string>();
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
 * @return Max read bytes (32KB default when no model context provided).
 * @internal
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
 * @param root_dir Project root directory.
 * @param config Filesystem configuration.
 * @param data_dir Path to bundled data directory.
 * @param model_context_bytes Model context window in bytes.
 * @internal
 * @version 2.1.4
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

    read_file_ = std::make_unique<ReadFileTool>(*this, data_dir);
    write_file_ = std::make_unique<WriteFileTool>(*this, data_dir);
    edit_file_ = std::make_unique<EditFileTool>(*this, data_dir);
    glob_ = std::make_unique<GlobTool>(*this, data_dir);
    grep_ = std::make_unique<GrepTool>(*this, data_dir);
    list_dir_ = std::make_unique<ListDirectoryTool>(
        *this, data_dir);

    register_tool(read_file_.get());
    register_tool(write_file_.get());
    register_tool(edit_file_.get());
    register_tool(glob_.get());
    register_tool(grep_.get());
    register_tool(list_dir_.get());

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
 * @brief Destructor (default, unique_ptr cleanup).
 * @internal
 * @version 1.8.5
 */
FilesystemServer::~FilesystemServer() = default;

/**
 * @brief read_file always executes (updates FileAccessTracker).
 * @param tool_name Tool name to check.
 * @return true for "read_file", false otherwise.
 * @internal
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
 * @return true on success, false if path is not a directory.
 * @internal
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
 * @return Root directory path reference.
 * @internal
 * @version 1.8.5
 */
const fs::path& FilesystemServer::root_dir() const {
    return root_dir_;
}

/**
 * @brief Get the file access tracker.
 * @return Mutable tracker reference.
 * @internal
 * @version 1.8.5
 */
FileAccessTracker& FilesystemServer::tracker() {
    return tracker_;
}

/**
 * @brief Get the ignore matcher (#15, v2.1.4).
 * @return Read-only matcher reference.
 * @internal
 * @version 2.1.4
 */
const IgnoreMatcher& FilesystemServer::ignore() const {
    return ignore_;
}

/**
 * @brief Get the filesystem config.
 * @return Config reference.
 * @internal
 * @version 1.8.5
 */
const FilesystemConfig& FilesystemServer::config() const {
    return config_;
}

/**
 * @brief Get max read bytes for size gate.
 * @return Max bytes, or 0 for unlimited.
 * @internal
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
 * @param requested User-requested path string.
 * @return Resolved canonical path.
 * @throws std::runtime_error if path escapes root.
 * @internal
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
