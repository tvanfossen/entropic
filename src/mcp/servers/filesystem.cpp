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
#include <entropic/types/run_scope.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
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
 * @brief Record that a session read a file, with its content hash.
 *
 * The tracker is what makes read-before-write enforceable: write_file
 * and edit_file refuse an existing file with no recorded read.
 *
 * gh#158 (v2.13.0): recorded under the READING session's key, so the
 * read unlocks that session's writes and nobody else's; the map is
 * locked because concurrent sessions record from several run threads.
 *
 * @param session Session key the read belongs to.
 * @param path Canonical file path.
 * @param hash Content hash at time of read.
 * @req REQ-MCP-021
 * @req REQ-LOOP-009
 * @version 2.13.0 [reviewed]
 */
void FileAccessTracker::record_read(const std::string& session,
                                    const std::string& path,
                                    size_t hash) {
    reads_.with(session, [&](auto& reads) { reads[path] = hash; });
    logger->info("Tracked read: session='{}' {}", session, path);
}


/**
 * @brief Check whether a session has read a file.
 *
 * gh#158 (v2.13.0): consults ONLY `session`'s reads. A lookup never
 * creates an entry, so asking on behalf of a session that read nothing
 * does not grow the map.
 *
 * @param session Session key asking.
 * @param path Canonical file path.
 * @return true when this session recorded a read of this path,
 *         regardless of whether the content has since changed.
 * @req REQ-MCP-021
 * @req REQ-LOOP-009
 * @version 2.13.0 [reviewed]
 */
bool FileAccessTracker::was_read(const std::string& session,
                                 const std::string& path) const {
    return reads_.peek(session, [&](const auto* reads) {
        return reads != nullptr && reads->count(path) > 0;
    });
}

/**
 * @brief Forget every read a session recorded (gh#158).
 *
 * Called when the session's conversation ends — the model no longer
 * holds the content it read, so a later write must read again.
 *
 * @param session Session key.
 * @return true when the session had recorded reads.
 * @req REQ-MCP-021
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
bool FileAccessTracker::release_session(const std::string& session) {
    return reads_.release(session);
}

/**
 * @brief Sessions currently holding recorded reads (gh#158).
 * @return Session count.
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
std::size_t FileAccessTracker::session_count() const {
    return reads_.session_count();
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
 * @brief Whether `path` lies in the subtree rooted at `base`.
 *
 * `lexically_relative`, never string prefix, so `/home/u/project` does
 * not contain `/home/u/projectile` and `/opt/data` does not contain
 * `/opt/database`. `base` itself is inside its own subtree. Both paths
 * are expected canonical — the caller canonicalises.
 *
 * @param path Canonical candidate path.
 * @param base Canonical subtree root.
 * @return true when `path` is `base` or below it.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
bool path_within(const fs::path& path, const fs::path& base) {
    fs::path rel = path.lexically_relative(base);
    return !rel.empty()
        && *rel.begin() != fs::path("..")
        && rel != fs::path("..");
}

/**
 * @brief Canonicalise a configured list entry without throwing.
 * @param entry Configured path (absolute after load).
 * @return weakly_canonical form, or the lexically normal form when the
 *         filesystem cannot be queried.
 * @dg_internal
 * @version 2.13.0
 */
fs::path canonical_entry(const fs::path& entry) {
    std::error_code ec;
    auto c = fs::weakly_canonical(entry, ec);
    return ec ? entry.lexically_normal() : c;
}

/**
 * @brief The first list entry whose subtree holds `resolved`.
 *
 * Entries are canonicalised at CHECK time, not load time, so a symlinked
 * entry is matched by where it points now — a deny entry that is a
 * symlink denies its target however the model spells the path.
 *
 * @param entries Configured `outside_root_allow` / `outside_root_deny`.
 * @param resolved Canonical requested path.
 * @return The matching canonical entry, or nullopt.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
std::optional<fs::path> first_containing(
    const std::vector<fs::path>& entries, const fs::path& resolved) {
    std::optional<fs::path> hit;
    for (const auto& e : entries) {
        auto base = canonical_entry(e);
        if (path_within(resolved, base)) {
            hit = base;
            break;
        }
    }
    return hit;
}

/**
 * @brief The YAML spelling of an outside-root mode, for logs.
 * @param mode Configured mode.
 * @return "false", "true" or "optional".
 * @dg_internal
 * @version 2.13.0
 */
const char* outside_root_mode_name(OutsideRootAccess mode) {
    const char* name = "optional";
    if (mode == OutsideRootAccess::refuse) { name = "false"; }
    if (mode == OutsideRootAccess::allow) { name = "true"; }
    return name;
}

/**
 * @brief "read" or "write", for approval requests and messages.
 * @param access Access kind.
 * @return Lower-case verb.
 * @dg_internal
 * @version 2.13.0
 */
const char* access_verb(PathAccess access) {
    return access == PathAccess::write ? "write" : "read";
}

/**
 * @brief The refusal text for an approver verdict other than `approved`.
 *
 * Each message leads with a stable type token the model and an operator's
 * log grep can both branch on, keeps the pre-2.13 "Path escapes project
 * root: <path>" phrase, and then says what to change — because a refusal
 * nobody can act on just becomes a retry loop.
 *
 * @param verdict `rejected` or `no_approver`.
 * @param req The request that was refused.
 * @return Human-readable, typed refusal message.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
std::string outside_root_refusal(OutsideRootVerdict verdict,
                                 const OutsideRootRequest& req) {
    std::string head = "Path escapes project root: " + req.path
        + " (root: " + req.root + ")";
    if (verdict == OutsideRootVerdict::rejected) {
        return "outside_root_rejected: " + head + " — the host's path "
               "approver refused this " + access_verb(req.access) + ".";
    }
    return "outside_root_approval_required: " + head + " — "
        + access_verb(req.access) + " access outside the project root "
        "needs approval (mcp.filesystem.allow_outside_root: optional) and "
        "no path approver is registered, so it is refused. Pre-approve "
        "the directory with mcp.filesystem.outside_root_allow, set "
        "allow_outside_root: true, or register an approver with "
        "entropic_set_path_approval_callback.";
}

/**
 * @brief Refuse a path under an `outside_root_deny` entry (logs, throws).
 * @param resolved Canonical requested path.
 * @param entry The canonical deny entry it falls under.
 * @param access Read or write.
 * @param root Canonical project root.
 * @throws std::runtime_error always, typed `outside_root_denied`.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
[[noreturn]] void refuse_denied(const fs::path& resolved,
                                const fs::path& entry, PathAccess access,
                                const fs::path& root) {
    logger->error("Path escape blocked by outside_root_deny: {} {} "
                  "(entry: {}, root: {})", access_verb(access),
                  resolved.string(), entry.string(), root.string());
    throw std::runtime_error(
        "outside_root_denied: " + resolved.string() + " is under "
        "mcp.filesystem.outside_root_deny entry " + entry.string()
        + " — refused regardless of allow_outside_root.");
}

/**
 * @brief Refuse an escape under `allow_outside_root: false` (logs, throws).
 *
 * The message is the pre-2.13 one, verbatim, so an explicit `false`
 * behaves — and reads — exactly as it always did.
 *
 * @param resolved Canonical requested path.
 * @param root Canonical project root.
 * @throws std::runtime_error always.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
[[noreturn]] void refuse_escape(const fs::path& resolved,
                                const fs::path& root) {
    logger->error("Path escape blocked: {} (root: {})",
                  resolved.string(), root.string());
    throw std::runtime_error(
        "Path escapes project root: " + resolved.string());
}

/**
 * @brief Log an outside-root access served without a prompt.
 * @param resolved Canonical requested path.
 * @param access Read or write.
 * @param allowed_by The `outside_root_allow` entry, or nullopt when
 *        `allow_outside_root: true` served it.
 * @dg_internal
 * @version 2.13.0
 */
void log_outside_root_served(const fs::path& resolved, PathAccess access,
                             const std::optional<fs::path>& allowed_by) {
    std::string why = allowed_by.has_value()
        ? "outside_root_allow entry " + allowed_by->string()
        : std::string("allow_outside_root: true");
    logger->info("Outside-root {} served ({}): {}", access_verb(access),
                 why, resolved.string());
}

/**
 * @brief Render a path list for a log line.
 * @param paths Paths.
 * @return `[a, b]`.
 * @dg_internal
 * @version 2.13.0
 */
std::string join_paths(const std::vector<fs::path>& paths) {
    std::string s;
    for (const auto& p : paths) {
        s += (s.empty() ? "" : ", ") + p.string();
    }
    return "[" + s + "]";
}

/**
 * @brief Warn about deny entries that lie inside the root.
 *
 * The lists govern OUTSIDE-root paths only; an operator who denies a
 * directory inside the root believes something is protected that is not.
 * Silence would be a silent no-op of a security setting.
 *
 * @param deny Configured `outside_root_deny`.
 * @param root Canonical project root.
 * @dg_internal
 * @version 2.13.0
 */
void warn_deny_entries_inside_root(const std::vector<fs::path>& deny,
                                   const fs::path& root) {
    for (const auto& e : deny) {
        if (path_within(canonical_entry(e), root)) {
            logger->warn("outside_root_deny entry {} is INSIDE the root {} "
                         "— the outside-root lists govern paths outside "
                         "the root only, so it has no effect there",
                         e.string(), root.string());
        }
    }
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
 * @brief A requested page: how many results to skip, how many to keep.
 * @dg_internal
 * @version 2.14.0
 */
struct Page {
    long offset = 0; ///< Results to skip (from a previous next_offset)
    long limit = 0;  ///< Results to keep
};

/**
 * @brief Read `offset`/`limit` from tool arguments (gh#202).
 *
 * Bounds are enforced here as well as by the executor's schema check,
 * because a handler can be reached without the executor (tests, the
 * plugin ABI). Out of range is a declared error, never a silent clamp.
 *
 * @param args Tool arguments.
 * @param default_limit Limit when `limit` is absent.
 * @param max_limit Largest accepted `limit`.
 * @param[out] page Parsed page.
 * @return "" on success, else a structured `invalid_args` error.
 * @req REQ-MCP-022
 * @version 2.14.0
 */
std::string read_page(const json& args, long default_limit, long max_limit,
                      Page& page) {
    page.offset = args.value<long>("offset", 0);
    page.limit = args.value("limit", default_limit);
    std::string err;
    // Written with '>' only: knots v1.4.3 reads a bare '<' here as a
    // template bracket and loses the function boundary.
    if (0 > page.offset || 1 > page.limit || page.limit > max_limit) {
        err = make_error("invalid_args",
            "offset must be >= 0 and limit 1.." + std::to_string(max_limit)
            + " (got offset " + std::to_string(page.offset) + ", limit "
            + std::to_string(page.limit) + ")");
    }
    return err;
}

/**
 * @brief Collects one page of results and reports how the page ended.
 *
 * Replaces v2.13.0's trailing `{"truncated":true}` sentinel inside a
 * result array — an undeclared shape — with declared fields: `truncated`,
 * `reason` (`limit` = more exist, continue at `next_offset`; `walk` = the
 * directory walk hit its cap) (gh#173/#202/#203).
 *
 * @dg_internal
 * @version 2.14.0
 */
class PagedResult {
public:
    /**
     * @brief Start collecting a page.
     * @param page Requested page.
     * @version 2.14.0
     */
    explicit PagedResult(Page page) : page_(page) {}

    /**
     * @brief Offer the next result in order.
     * @param item Result.
     * @return false once the page is full and a further result exists —
     *         the caller stops walking.
     * @dg_internal
     * @version 2.14.0
     */
    bool offer(json item) {
        bool keep_going = true;
        if (page_.offset > seen_) {
            ++seen_;
        } else if (static_cast<long>(items_.size()) >= page_.limit) {
            more_ = true;
            keep_going = false;
        } else {
            items_.push_back(std::move(item));
        }
        return keep_going;
    }

    /**
     * @brief Whether the page filled with results still to come.
     * @return true once offer() has returned false.
     * @dg_internal
     * @version 2.14.0
     */
    bool full() const { return more_; }

    /**
     * @brief Write the page into `result` under `key`, with its ending.
     * @param[in,out] result Result object.
     * @param key Array key ("paths", "matches", "entries").
     * @param walk_truncated Whether the walk stopped on its cap.
     * @dg_internal
     * @version 2.14.0
     */
    void finish(json& result, const std::string& key,
                bool walk_truncated) {
        const long returned = static_cast<long>(items_.size());
        result[key] = std::move(items_);
        result["truncated"] = more_ || walk_truncated;
        if (more_) {
            result["reason"] = "limit";
            result["next_offset"] = page_.offset + returned;
        } else if (walk_truncated) {
            result["reason"] = "walk";
        }
    }

private:
    Page page_;
    long seen_ = 0;
    bool more_ = false;
    json items_ = json::array(); ///< Collected page
};

/**
 * @brief How a path is shown to the model: root-relative when inside the
 *        root (what a citation and every path argument use), else
 *        absolute (gh#203).
 * @param root Canonical project root.
 * @param path Path to show.
 * @return Display string; "." for the root itself.
 * @dg_internal
 * @version 2.14.0
 */
std::string display_path(const fs::path& root, const fs::path& path) {
    return path_within(path, root)
        ? path.lexically_relative(root).generic_string()
        : path.string();
}

/**
 * @brief The window of a file one read_file call returns.
 * @dg_internal
 * @version 2.14.0
 */
struct ReadWindow {
    long offset = 1; ///< First line, 1-based
    long limit = 0;  ///< Max lines; 0 = until the byte budget is spent
    long budget = 0; ///< Max content bytes; <= 0 = unbounded
};

/**
 * @brief Accumulates one read_file window, line by line.
 * @dg_internal
 * @version 2.14.0
 */
struct WindowReader {
    const ReadWindow& w;          ///< Requested window
    json lines = json::array();   ///< Lines taken so far
    long bytes = 0;               ///< Content bytes taken
    bool more = false;            ///< A line past the window exists
    std::string err;              ///< size_exceeded, when set

    /**
     * @brief Consider line `n` for the window.
     * @param n 1-based line number.
     * @param line Line text.
     * @dg_internal
     * @version 2.14.0
     */
    void take(long n, const std::string& line) {
        const long size = static_cast<long>(line.size()) + 1;
        const bool full = (w.limit > 0 && static_cast<long>(lines.size()) >= w.limit)
            || (w.budget > 0 && bytes + size > w.budget);
        if (w.offset > n || more) { return; }
        if (full && lines.empty()) {
            err = make_error("size_exceeded", "Line " + std::to_string(n)
                + " is " + std::to_string(size) + " bytes, more than the "
                "whole read budget (" + std::to_string(w.budget) + ")");
        } else if (full) {
            more = true;
        } else {
            json entry;
            entry["line"] = n;
            entry["content"] = line;
            lines.push_back(std::move(entry));
            bytes += size;
        }
    }
};

/**
 * @brief Read a window of numbered lines, streaming (gh#201).
 *
 * Every line carries its 1-based number so a model that cites what it
 * read never counts. The whole file is walked to report `total`, but
 * only the window is held. A window stops at `limit` lines or when the
 * next line would overrun the byte budget; then `truncated` is true and
 * `next_offset` names where to resume. A single line larger than the
 * whole budget cannot be returned at all and is `size_exceeded`.
 *
 * @param path File to read.
 * @param display Path as shown to the model.
 * @param w Requested window.
 * @param[out] err Set to a structured error on failure.
 * @return The declared success object (see read_file.json).
 * @req REQ-MCP-021
 * @version 2.14.0
 */
json read_numbered_lines(const fs::path& path, const std::string& display,
                         const ReadWindow& w, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    WindowReader reader{w};
    std::string line;
    long n = 0;
    while (reader.err.empty() && std::getline(in, line)) {
        reader.take(++n, line);
    }
    err = reader.err;
    json result;
    result["path"] = display;
    result["total"] = n;
    result["offset"] = w.offset;
    result["truncated"] = reader.more;
    if (reader.more) {
        result["next_offset"] = w.offset + static_cast<long>(reader.lines.size());
    }
    result["lines"] = std::move(reader.lines);
    return result;
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
 * an existing one must have been read by THE CALLING SESSION (gh#158) —
 * the session the dispatch routed on, published to this thread by the
 * ToolExecutor. Another session's read of the same file does not count.
 *
 * @param tracker File access tracker.
 * @param path Canonical path string.
 * @return An empty string when the write may proceed; otherwise a
 *         structured `read_before_write` error naming the file, logged
 *         at warning level.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
std::string check_read_before_write(
    const FileAccessTracker& tracker,
    const std::string& path) {
    const auto session = current_run_session();
    if (fs::exists(path) && !tracker.was_read(session, path)) {
        logger->warn("Read-before-write violation: session='{}' {}",
                     session, path);
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
 * @param error_type Output: "no_match" or "multiple_matches" on failure.
 * @return Modified content, or nullopt on error.
 * @dg_internal
 * @version 2.14.0
 */
std::optional<std::string>
apply_str_replace(const std::string& content, const std::string& old_str, const std::string& new_str, bool replace_all, std::string& error_type) {

    int occurrences = count_occurrences(content, old_str);
    if (occurrences == 0) {
        error_type = "no_match";
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
 * @brief Number of lines in `content` as read_file numbers them.
 * @param content File content.
 * @return Line count (a final line without '\n' counts).
 * @dg_internal
 * @version 2.14.0
 */
long count_lines(const std::string& content) {
    long n = static_cast<long>(std::count(content.begin(), content.end(), '\n'));
    return (!content.empty() && content.back() != '\n') ? n + 1 : n;
}

/**
 * @brief Insert text AFTER line `after` (0 = start of file).
 *
 * v2.14.0 (gh#203): the schema always said AFTER; the code inserted
 * BEFORE, and `insert_line: 0` matched no line and wrote nothing while
 * reporting success. The caller validates `after` in 0..count_lines.
 * A file's missing final newline is preserved.
 *
 * @param content File content.
 * @param after Line the new text follows (0..line count).
 * @param new_str Text to insert (may span lines).
 * @return Modified content.
 * @req REQ-MCP-021
 * @version 2.14.0
 */
std::string apply_insert(const std::string& content,
                         long after,
                         const std::string& new_str) {
    size_t pos = 0;
    for (long line = 0; line < after; ++line) {
        auto nl = content.find('\n', pos);
        pos = (nl == std::string::npos) ? content.size() : nl + 1;
    }
    std::string text = new_str;
    bool at_unterminated_end = pos == content.size() && !content.empty()
        && content.back() != '\n';
    if (at_unterminated_end) {
        text = "\n" + text;
    } else if (text.empty() || text.back() != '\n') {
        text += '\n';
    }
    return content.substr(0, pos) + text + content.substr(pos);
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
 * @brief Collect files matching a glob into one page, honoring
 *        IgnoreMatcher and brace-expanded patterns.
 *
 * Issues #13/#15 (v2.1.4): brace expansion + ignore filtering.
 * v2.14.0 (gh#203): paths are root-relative and paged; the caller learns
 * whether the page filled or the walk ran out of budget.
 *
 * @param root Starting directory for traversal (the project root).
 * @param pattern Glob pattern (may contain `{a,b,c}`).
 * @param ignore Optional ignore matcher (nullptr disables filtering).
 * @param[in,out] budget Entry budget (gh#161).
 * @param[in,out] page Page being collected.
 * @req REQ-MCP-022
 * @req REQ-MCP-021
 * @version 2.14.0
 */
void collect_glob_matches(
    const fs::path& root,
    const std::string& pattern,
    const IgnoreMatcher* ignore,
    WalkBudget& budget,
    PagedResult& page) {

    auto patterns = expand_braces(pattern);
    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied);

    for (auto& entry : it) {
        auto action = classify_glob_entry(entry, root, patterns, ignore);
        if (action == EntryAction::kSkipPrune) {
            it.disable_recursion_pending();
        } else if (action == EntryAction::kTake
                   && !page.offer(display_path(root, entry.path()))) {
            break;
        }
        if (walk_budget_spent(budget)) { break; }
    }
}

/**
 * @brief Whether a file looks binary: a NUL byte in its first 8 KiB.
 *
 * grep's description always said binary files were skipped; until
 * v2.14.0 nothing checked (gh#203).
 *
 * @param path File.
 * @return true when the file should not be searched as text.
 * @dg_internal
 * @version 2.14.0
 */
bool looks_binary(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    char buf[8192];
    in.read(buf, sizeof(buf));
    return std::memchr(buf, '\0', static_cast<size_t>(in.gcount())) != nullptr;
}

/**
 * @brief Search a single file for regex matches, feeding one page.
 * @param path File path.
 * @param display Path as shown to the model.
 * @param re Compiled regex.
 * @param[in,out] page Page being collected.
 * @return false once the page is full (stop the search).
 * @req REQ-MCP-022
 * @version 2.14.0
 */
bool grep_file(const fs::path& path, const std::string& display,
               const std::regex& re, PagedResult& page) {
    std::ifstream in(path);
    std::string line;
    long line_num = 0;
    bool keep_going = !looks_binary(path);
    while (keep_going && std::getline(in, line)) {
        ++line_num;
        if (std::regex_search(line, re)) {
            json m;
            m["path"] = display;
            m["line"] = line_num;
            m["content"] = line;
            keep_going = page.offer(std::move(m));
        }
    }
    return !page.full();
}

/**
 * @brief Convert a directory entry to a JSON object.
 * @param entry Directory entry.
 * @param base The listed directory; `name` is relative to it.
 * @return JSON with name, type ("file"|"directory") and size.
 * @dg_internal
 * @version 2.14.0
 */
json entry_to_json(const fs::directory_entry& entry, const fs::path& base) {
    json j;
    j["name"] = entry.path().lexically_relative(base).generic_string();
    bool dir = entry.is_directory();
    j["type"] = dir ? "directory" : "file";
    j["size"] = (!dir && entry.is_regular_file())
        ? static_cast<int64_t>(entry.file_size()) : 0;
    return j;
}

/**
 * @brief Collect directory entries, optionally recursive, sorted by name.
 *
 * v2.14.0: charged to the walk budget like glob and grep (a recursive
 * listing of a large tree was unbounded), and sorted so `offset` pages
 * through a stable order.
 *
 * @param dir Directory to list.
 * @param recursive Whether to recurse (tool default false, gh#116).
 * @param max_depth Depth below `dir` to descend (tool default 3; 0 =
 *                  direct children only).
 * @param[in,out] budget Walk budget.
 * @return Entries sorted by relative name.
 * @req REQ-MCP-021
 * @version 2.14.0
 */
std::vector<json> collect_entries(const fs::path& dir,
                                  bool recursive,
                                  int max_depth,
                                  WalkBudget& budget) {
    std::vector<json> entries;
    auto it = fs::recursive_directory_iterator(
        dir, fs::directory_options::skip_permission_denied);
    for (auto& entry : it) {
        if (!recursive || it.depth() >= max_depth) {
            it.disable_recursion_pending();
        }
        entries.push_back(entry_to_json(entry, dir));
        if (walk_budget_spent(budget)) { break; }
    }
    std::sort(entries.begin(), entries.end(),
              [](const json& a, const json& b) { return a["name"] < b["name"]; });
    return entries;
}

/**
 * @brief Apply string replacement mode of edit_file.
 * @param args Parsed JSON arguments.
 * @param content Current file content.
 * @param out Modified content (output).
 * @param[out] ok The success object's mode fields.
 * @return An empty string on success; otherwise a structured
 *         `no_match` error when old_string is absent, or
 *         `multiple_matches` (with the use-replace_all hint) when it is
 *         ambiguous — an ambiguous edit is refused, never guessed.
 * @req REQ-MCP-021
 * @version 2.14.0
 */
std::string do_str_replace(const json& args,
                           const std::string& content,
                           std::string& out, json& ok) {
    auto old_str = args.at("old_string").get<std::string>();
    auto new_str = args.at("new_string").get<std::string>();
    bool replace_all = args.value("replace_all", false);

    std::string error_type;
    auto result = apply_str_replace(
        content, old_str, new_str, replace_all, error_type);
    if (!result.has_value()) {
        auto msg = (error_type == "multiple_matches")
            ? "old_string found multiple times — add context or use replace_all"
            : "old_string not found in file";
        return make_error(error_type, msg);
    }
    out = result.value();
    ok["mode"] = "str_replace";
    ok["replacements"] = replace_all ? count_occurrences(content, old_str) : 1;
    ok["message"] = "Edit applied";
    return "";
}

/**
 * @brief Apply line insertion mode of edit_file.
 * @param args Parsed JSON arguments.
 * @param content Current file content.
 * @param out Modified content (output).
 * @param[out] ok The success object's mode fields.
 * @return "" on success, else a structured `out_of_range` error when
 *         insert_line is not in 0..line count — never a silent no-op.
 * @req REQ-MCP-021
 * @version 2.14.0
 */
std::string do_insert(const json& args,
                      const std::string& content,
                      std::string& out, json& ok) {
    const long after = args.at("insert_line").get<long>();
    const long total = count_lines(content);
    if (after < 0 || after > total) {
        return make_error("out_of_range", "insert_line must be 0.."
            + std::to_string(total) + " for this file (got "
            + std::to_string(after) + ")");
    }
    out = apply_insert(content, after, args.at("new_string").get<std::string>());
    ok["mode"] = "insert";
    ok["inserted_after"] = after;
    ok["message"] = "Text inserted";
    return "";
}

/**
 * @brief Apply an edit operation and return result JSON or error.
 * @param args Parsed arguments — exactly one of `old_string` (replace
 *             mode) or `insert_line` (insert mode).
 * @param resolved Resolved (root-confined) file path.
 * @param display Path as shown to the model.
 * @return The mode's declared success object when the edit applied and
 *         was written back; otherwise the mode's structured error, or
 *         `invalid_args` when not exactly one mode was selected. The
 *         file is left untouched on every error path.
 * @req REQ-MCP-021
 * @version 2.14.0
 */
std::string apply_edit(const json& args,
                       const std::filesystem::path& resolved,
                       const std::string& display) {
    const bool replace = args.contains("old_string");
    if (replace == args.contains("insert_line")) {
        return make_error("invalid_args", "edit_file takes exactly one of "
            "old_string (STR_REPLACE) or insert_line (INSERT)");
    }
    auto content = read_file_contents(resolved);
    std::string edited;
    json ok;
    auto out = replace ? do_str_replace(args, content, edited, ok)
                       : do_insert(args, content, edited, ok);
    if (out.empty()) {
        write_file_contents(resolved, edited);
        logger->info("Edited file: {} ({})", display, ok["mode"].get<std::string>());
        ok["path"] = display;
        out = ok.dump();
    }
    return out;
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
 * @brief Run pre-read gates (existence, directory, ignore).
 *
 * Path-relative ignore matching uses the server's IgnoreMatcher (#15,
 * v2.1.4). v2.14.0: the whole-file size gate is gone — a large file is
 * read a window at a time (gh#201); `max_read_bytes` is now the window's
 * byte budget.
 *
 * @param server Owning server (source of the root and ignore rules).
 * @param resolved Already root-confined path.
 * @param display Path as shown to the model.
 * @return An empty string when the read may proceed; otherwise a
 *         structured error — `not_found` (whose message names
 *         list_directory, gh#124), `is_directory` for a directory
 *         argument (gh#116), or `ignored` for a path excluded by
 *         .gitignore/.explorerignore.
 * @req REQ-MCP-021
 * @req REQ-MCP-022
 * @version 2.14.0
 */
std::string check_read_gates(FilesystemServer& server,
                             const fs::path& resolved,
                             const std::string& display) {
    std::string err;
    auto rel = fs::relative(resolved, server.root_dir()).generic_string();
    if (!fs::exists(resolved)) {
        err = make_error("not_found",
            "File not found: " + display
            + ". Use list_directory(\".\") to see available files"
              " in the working directory, or verify the path is"
              " relative to the configured root.");
    } else if (fs::is_directory(resolved)) {
        err = make_error("is_directory",
            "Path is a directory, not a file: " + display);
    } else if (!rel.empty() && server.ignore().is_ignored(rel, false)) {
        err = make_error("ignored",
            "Path '" + rel + "' is excluded by .gitignore or "
            ".explorerignore.");
    }
    return err;
}

/**
 * @brief The window a read_file call asks for, validated.
 * @param args Tool arguments.
 * @param budget Byte budget (max_read_bytes).
 * @param[out] w Parsed window.
 * @return "" or a declared `invalid_args` error.
 * @dg_internal
 * @version 2.14.0
 */
std::string read_window(const json& args, long budget, ReadWindow& w) {
    w.offset = args.value<long>("offset", 1);
    w.limit = args.value<long>("limit", 0);
    w.budget = budget;
    return (1 > w.offset || 0 > w.limit)
        ? make_error("invalid_args", "offset must be >= 1 and limit >= 1")
        : std::string{};
}

/**
 * @brief Read the requested window and record the read for the session.
 * @param server Owning server (budget, tracker).
 * @param resolved File to read (already gated).
 * @param display Path as shown to the model.
 * @param args Tool arguments (offset, limit).
 * @param[out] err Declared error, when the window is invalid or a line
 *             exceeds the budget.
 * @return The declared success object (meaningful only when err is "").
 * @dg_internal
 * @version 2.14.0
 */
json read_and_track(FilesystemServer& server, const fs::path& resolved,
                    const std::string& display, const json& args,
                    std::string& err) {
    ReadWindow w;
    err = read_window(args, server.max_read_bytes(), w);
    json result;
    if (err.empty()) { result = read_numbered_lines(resolved, display, w, err); }
    if (err.empty()) {
        server.tracker().record_read(current_run_session(), resolved.string(),
                                     hash_content(result["lines"].dump()));
        logger->info("Read file: {} lines {}..{} of {} (truncated={})",
                     resolved.string(), w.offset,
                     w.offset + static_cast<long>(result["lines"].size()) - 1,
                     result["total"].get<long>(), result["truncated"].get<bool>());
    }
    return result;
}

/**
 * @brief Execute read_file: resolve, gate, read a numbered window, track.
 *
 * A successful read records the read in the FileAccessTracker — which is
 * why read_file must always execute and opts out of duplicate detection.
 * A partial (windowed) read counts as a read for read-before-write.
 *
 * v2.13.0: resolved as a READ for the outside-root policy; gh#158: the
 * read is recorded for the calling session only. v2.14.0 (gh#201): every
 * line carries its number, and long files are paged.
 *
 * @param args_json JSON with "path" and optional "offset"/"limit".
 * @return A ServerResponse with no directives whose result is the
 *         declared success object or a declared structured error.
 * @req REQ-MCP-021
 * @version 2.14.0
 */
ServerResponse ReadFileTool::execute(const std::string& args_json) {
    auto args = json::parse(args_json);
    auto resolved = server_.resolve_path(args.at("path").get<std::string>(),
                                         PathAccess::read, name());
    auto display = display_path(server_.root_dir(), resolved);
    auto err = check_read_gates(server_, resolved, display);
    json result;
    if (err.empty()) { result = read_and_track(server_, resolved, display, args, err); }
    return {err.empty() ? result.dump() : err, {}};
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
 * v2.13.0: resolved as a WRITE, so an approver sees "wants to write".
 *
 * @param args_json JSON with "path" and "content" keys.
 * @return A ServerResponse with no directives whose result is either a
 *         success JSON naming the root-relative path and bytes written, or the
 *         structured `read_before_write` error — in which case nothing
 *         is written.
 * @req REQ-MCP-021
 * @version 2.14.0
 */
ServerResponse WriteFileTool::execute(
    const std::string& args_json) {

    auto args = json::parse(args_json);
    auto requested = args.at("path").get<std::string>();
    auto content = args.at("content").get<std::string>();
    auto resolved = server_.resolve_path(requested, PathAccess::write,
                                         name());
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
    result["path"] = display_path(server_.root_dir(), resolved);
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
 *
 * v2.13.0: resolved as a WRITE — an edit modifies the file.
 *
 * @param args_json JSON arguments.
 * @return ServerResponse whose result is the declared success object or
 *         a declared structured error.
 * @dg_internal
 * @version 2.14.0
 */
ServerResponse EditFileTool::execute(const std::string& args_json) {
    auto args = json::parse(args_json);
    auto requested = args.at("path").get<std::string>();
    auto resolved = server_.resolve_path(requested, PathAccess::write,
                                         name());
    auto violation = check_read_before_write(
        server_.tracker(), resolved.string());
    if (!violation.empty()) {
        return {violation, {}};
    }
    return {apply_edit(args, resolved,
                       display_path(server_.root_dir(), resolved)), {}};
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
 * @brief Execute glob: one page of root-relative paths matching a pattern.
 *
 * Issues #13/#15 (v2.1.4): brace expansion, ignore filtering.
 * v2.14.0 (gh#203): declared `{paths, truncated, reason?, next_offset?}`
 * instead of an array whose last element might be a sentinel object.
 *
 * @param args_json JSON with "pattern" and optional "limit"/"offset".
 * @return ServerResponse with the declared result or `invalid_args`.
 * @req REQ-MCP-022
 * @version 2.14.0
 */
ServerResponse GlobTool::execute(const std::string& args_json) {
    auto args = json::parse(args_json);
    auto pattern = args.at("pattern").get<std::string>();
    Page requested;
    auto err = read_page(args, 500, 5000, requested);
    if (!err.empty()) { return {err, {}}; }

    WalkBudget budget{server_.config().max_walk_entries, 0, false};
    PagedResult page(requested);
    collect_glob_matches(server_.root_dir(), pattern, &server_.ignore(),
                         budget, page);
    json result;
    page.finish(result, "paths", budget.truncated);
    logger->info("Glob '{}': {} paths, {} entries walked, truncated={}{}",
                 pattern, result["paths"].size(), budget.visited,
                 result["truncated"].get<bool>(),
                 budget.truncated ? " (walk cap — narrow the pattern)" : "");
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
 * @brief Walk `start` applying the glob/ignore filter and grep files.
 * @param root Project root (patterns and shown paths are relative to it).
 * @param start Directory to walk (the root, or the `path` argument).
 * @param file_patterns Brace-expanded glob patterns.
 * @param re Compiled content regex.
 * @param ignore Ignore matcher.
 * @param[in,out] budget Entry budget (gh#161) — grep needs it MORE than
 *                glob does, since it also opens every matching file.
 * @param[in,out] page Page being collected.
 * @req REQ-MCP-022
 * @version 2.14.0
 */
static void grep_search(
    const fs::path& root, const fs::path& start,
    const std::vector<std::string>& file_patterns,
    const std::regex& re, const IgnoreMatcher& ignore,
    WalkBudget& budget, PagedResult& page) {
    auto it = fs::recursive_directory_iterator(
        start, fs::directory_options::skip_permission_denied);
    for (auto& entry : it) {
        auto action = classify_glob_entry(entry, root, file_patterns,
                                          &ignore);
        if (action == EntryAction::kSkipPrune) {
            it.disable_recursion_pending();
        } else if (action == EntryAction::kTake
                   && !grep_file(entry.path(),
                                 display_path(root, entry.path()), re, page)) {
            break;
        }
        if (walk_budget_spent(budget)) { break; }
    }
}

/**
 * @brief Search `start` — one file, or a tree under the glob filter.
 * @param server Owning server (root, ignore rules).
 * @param start Resolved `path` argument.
 * @param args Tool arguments (glob).
 * @param re Compiled regex.
 * @param[in,out] budget Walk budget.
 * @param[in,out] page Page being collected.
 * @dg_internal
 * @version 2.14.0
 */
static void grep_target(FilesystemServer& server, const fs::path& start,
                        const json& args, const std::regex& re,
                        WalkBudget& budget, PagedResult& page) {
    const auto& root = server.root_dir();
    if (fs::is_regular_file(start)) {
        grep_file(start, display_path(root, start), re, page);
    } else {
        grep_search(root, start,
                    expand_braces(args.value("glob", std::string("*"))),
                    re, server.ignore(), budget, page);
    }
}

/**
 * @brief Validate a grep call: paging, regex, and that `path` exists.
 * @param args Tool arguments.
 * @param start Resolved `path` argument.
 * @param root Project root (for the message).
 * @param[out] requested Parsed page.
 * @param[out] re Compiled regex.
 * @return "" or the first declared error (`invalid_args`,
 *         `invalid_regex`, `not_found`).
 * @dg_internal
 * @version 2.14.0
 */
static std::string grep_preflight(const json& args, const fs::path& start,
                                  const fs::path& root, Page& requested,
                                  std::regex& re) {
    std::string err = read_page(args, 100, 1000, requested);
    re = compile_grep_or_error(args.at("pattern").get<std::string>(), err);
    if (err.empty() && !fs::exists(start)) {
        err = make_error("not_found", "No such file or directory: "
                         + display_path(root, start));
    }
    return err;
}

/**
 * @brief Execute grep: compile, then search one file or walk a tree.
 *
 * v2.14.0 (gh#173/#202/#203): declared envelope
 * `{matches, truncated, reason?, next_offset?}`; `limit`/`offset` page
 * past what used to be a silent 100-match cap; `path` aims at one file
 * or directory; binary files are skipped.
 *
 * @param args_json JSON with "pattern" and optional "glob" (default
 *                  "*"), "path", "limit" (default 100), "offset".
 * @return A ServerResponse whose result is the declared success object,
 *         or `invalid_regex`, `not_found` (no such `path`) or
 *         `invalid_args`.
 * @req REQ-MCP-022
 * @req REQ-MCP-021
 * @version 2.14.0
 */
ServerResponse GrepTool::execute(const std::string& args_json) {
    auto args = json::parse(args_json);
    auto start = server_.resolve_path(args.value("path", std::string(".")),
                                      PathAccess::read, name());
    Page requested;
    std::regex re;
    auto err = grep_preflight(args, start, server_.root_dir(), requested, re);
    if (!err.empty()) { return {err, {}}; }

    WalkBudget budget{server_.config().max_walk_entries, 0, false};
    PagedResult page(requested);
    grep_target(server_, start, args, re, budget, page);
    json result;
    page.finish(result, "matches", budget.truncated);
    logger->info("Grep '{}' in {}: {} matches, {} entries walked, truncated={}",
                 args.at("pattern").get<std::string>(), start.string(),
                 result["matches"].size(), budget.visited,
                 result["truncated"].get<bool>());
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
 * @brief Collect one page of a directory listing into `result`.
 * @param dir Directory to list.
 * @param args Tool arguments (recursive, max_depth).
 * @param requested Requested page.
 * @param max_walk Walk cap (filesystem.max_walk_entries).
 * @param[in,out] result Result object (gets entries + ending).
 * @dg_internal
 * @version 2.14.0
 */
static void list_page(const fs::path& dir, const json& args, Page requested,
                      int max_walk, json& result) {
    WalkBudget budget{max_walk, 0, false};
    PagedResult page(requested);
    for (auto& e : collect_entries(dir, args.value("recursive", false),
                                   args.value("max_depth", 3), budget)) {
        if (!page.offer(std::move(e))) { break; }
    }
    page.finish(result, "entries", budget.truncated);
}

/**
 * @brief Execute list_directory: one page of a directory's entries.
 *
 * The optional parameters carry documented defaults (gh#116): `path`
 * defaults to the project root, `recursive` to false, `max_depth` to 3.
 * v2.13.0: resolved as a READ for the outside-root policy. v2.14.0
 * (gh#203): the declared `{path, entries, truncated, ...}` object instead
 * of a bare array; recursive names are relative to the listed directory
 * (they were bare file names, ambiguous across subdirectories).
 *
 * @param args_json JSON with optional "path", "recursive", "max_depth",
 *                  "limit" (default 500) and "offset" — all may be omitted.
 * @return A ServerResponse whose result is the declared success object,
 *         or `not_directory` / `invalid_args`.
 * @req REQ-MCP-021
 * @version 2.14.0
 */
ServerResponse ListDirectoryTool::execute(
    const std::string& args_json) {

    auto args = json::parse(args_json);
    auto resolved = server_.resolve_path(args.value("path", std::string(".")),
                                         PathAccess::read, name());
    auto display = display_path(server_.root_dir(), resolved);
    Page requested;
    auto err = read_page(args, 500, 5000, requested);
    if (err.empty() && !fs::is_directory(resolved)) {
        err = make_error("not_directory", "Not a directory: " + display);
    }
    if (!err.empty()) { return {err, {}}; }

    json result;
    result["path"] = display;
    list_page(resolved, args, requested, server_.config().max_walk_entries, result);
    logger->info("Listed {}: {} entries, truncated={}", display,
                 result["entries"].size(), result["truncated"].get<bool>());
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
 * @version 2.13.0
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
    log_outside_root_policy();  // v2.13.0: the posture is observable
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
 * @version 2.13.0
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
    // v2.13.0: "outside the root" just moved; say what that now means.
    log_outside_root_policy();
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
 *         read_file (which records) and write_file (which enforces),
 *         both under the calling session's key (gh#158).
 * @req REQ-MCP-021
 * @version 1.8.5
 */
FileAccessTracker& FilesystemServer::tracker() {
    return tracker_;
}

/**
 * @brief Release a session's recorded reads (gh#158).
 * @param key Session key.
 * @return true when the session had recorded reads.
 * @req REQ-MCP-021
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
bool FilesystemServer::release_session(const std::string& key) {
    return tracker_.release_session(key);
}

/**
 * @brief Sessions whose reads this server tracks (gh#158).
 * @return Session count.
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
std::size_t FilesystemServer::session_count() const {
    return tracker_.session_count();
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
 * @brief Resolve a path and apply the outside-root policy.
 *
 * Resolves relative paths against root_dir_ and canonicalizes. A result
 * under the root is served. One outside it goes to
 * authorize_outside_root, which throws on any refusal.
 *
 * Containment uses lexically_relative so that "/home/user/project"
 * does not falsely contain "/home/user/projectile" via string-prefix —
 * and the SAME test decides the outside-root allow/deny subtrees.
 *
 * The single confinement point every path-taking filesystem tool goes
 * through, so no tool can reach outside the project without the policy
 * saying so.
 *
 * @param requested User-requested path string (absolute or relative to
 *                  the root).
 * @param access Whether the calling tool reads or writes the path.
 * @param tool Bare calling tool name (e.g. "write_file").
 * @return The canonical path when it lies under the root, or when the
 *         outside-root policy served it.
 * @throws std::runtime_error when the policy refuses the path — thrown,
 *         so MCPServerBase's barrier returns it as a tool error AND skips
 *         the ContextAnchor a refused read must not get.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
fs::path FilesystemServer::resolve_path(
    const std::string& requested, PathAccess access,
    const std::string& tool) const {

    fs::path req_path(requested);
    fs::path resolved = req_path.is_absolute()
        ? fs::weakly_canonical(req_path)
        : fs::weakly_canonical(root_dir_ / req_path);

    if (!path_within(resolved, root_dir_)) {
        authorize_outside_root(resolved, access, tool);
    }
    return resolved;
}

/**
 * @brief Decide one path that resolved outside the root (v2.13.0).
 *
 * Precedence, highest first: `outside_root_deny` refuses; then
 * `outside_root_allow` serves without a prompt; then
 * `allow_outside_root` — `true` serves, `false` refuses with the
 * pre-2.13 message, `optional` asks the approver.
 *
 * `permissions.auto_approve` is deliberately not consulted: it skips
 * per-TOOL prompts and says nothing about the filesystem boundary.
 *
 * @param resolved Canonical path outside the root.
 * @param access Read or write.
 * @param tool Bare tool name.
 * @throws std::runtime_error when refused, each kind logged.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
void FilesystemServer::authorize_outside_root(
    const fs::path& resolved, PathAccess access,
    const std::string& tool) const {
    auto denied = first_containing(config_.outside_root_deny, resolved);
    auto allowed = first_containing(config_.outside_root_allow, resolved);
    const auto mode = config_.allow_outside_root;
    if (denied.has_value()) {
        refuse_denied(resolved, *denied, access, root_dir_);
    } else if (allowed.has_value() || mode == OutsideRootAccess::allow) {
        log_outside_root_served(resolved, access, allowed);
    } else if (mode == OutsideRootAccess::refuse) {
        refuse_escape(resolved, root_dir_);
    } else {
        ask_outside_root_approver(resolved, access, tool);
    }
}

/**
 * @brief Put one outside-root access to the host's approver (v2.13.0).
 *
 * The approver pair is copied under the lock and called outside it: a
 * host prompt can block for as long as a person takes to answer, and
 * must not hold up a concurrent install. No approver means refuse —
 * `optional` never fails open.
 *
 * @param resolved Canonical path outside the root.
 * @param access Read or write.
 * @param tool Bare tool name.
 * @throws std::runtime_error unless the verdict is `approved`, with a
 *         typed `outside_root_rejected` / `outside_root_approval_required`
 *         message.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
void FilesystemServer::ask_outside_root_approver(
    const fs::path& resolved, PathAccess access,
    const std::string& tool) const {
    OutsideRootApprover fn = nullptr;
    void* ud = nullptr;
    {
        std::lock_guard<std::mutex> lock(approver_mutex_);
        fn = approver_;
        ud = approver_data_;
    }
    OutsideRootRequest req{resolved.string(), root_dir_.string(),
                           name() + "." + tool, access};
    auto verdict = fn != nullptr ? fn(req, ud)
                                 : OutsideRootVerdict::no_approver;
    if (verdict == OutsideRootVerdict::approved) {
        logger->info("Outside-root {} APPROVED by host: {} ({})",
                     access_verb(access), req.path, req.tool);
    } else {
        auto msg = outside_root_refusal(verdict, req);
        logger->error("Path escape blocked: {}", msg);
        throw std::runtime_error(msg);
    }
}

/**
 * @brief Install the approver consulted under `optional` (v2.13.0).
 * @param fn Approver, or nullptr to clear.
 * @param user_data Forwarded to `fn`.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
void FilesystemServer::set_outside_root_approver(
    OutsideRootApprover fn, void* user_data) {
    std::lock_guard<std::mutex> lock(approver_mutex_);
    approver_ = fn;
    approver_data_ = user_data;
    logger->info("Outside-root approver {} (root: {})",
                 fn != nullptr ? "installed" : "cleared",
                 root_dir_.string());
}

/**
 * @brief Log the outside-root policy; warn on entries with no effect.
 *
 * Observability for a security posture: the mode and both lists are
 * logged whenever the root is (re)set. A `true` mode is a WARN, because
 * it is the one setting that serves the whole disk. A deny entry inside
 * the root is warned about, because the lists govern OUTSIDE-root paths
 * only and an operator who wrote it believes something is protected.
 *
 * @req REQ-MCP-021
 * @version 2.13.0
 */
void FilesystemServer::log_outside_root_policy() const {
    const auto& deny = config_.outside_root_deny;
    if (config_.allow_outside_root == OutsideRootAccess::allow) {
        logger->warn("Outside-root policy: allow_outside_root=true — every "
                     "path outside {} is served (deny={})",
                     root_dir_.string(), join_paths(deny));
    } else {
        logger->info("Outside-root policy: allow_outside_root={} allow={} "
                     "deny={} (root: {})",
                     outside_root_mode_name(config_.allow_outside_root),
                     join_paths(config_.outside_root_allow),
                     join_paths(deny), root_dir_.string());
    }
    warn_deny_entries_inside_root(deny, root_dir_);
}

} // namespace entropic
