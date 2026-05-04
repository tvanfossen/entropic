// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file ignore_matcher.cpp
 * @brief Gitignore + explorerignore implementation (#15, v2.1.4).
 *
 * Pattern compilation is the only non-trivial part. Each gitignore
 * line becomes a Rule whose regex source matches against the path
 * relative to the rule's anchor base, in forward-slash form.
 *
 * Conversion rules (gitignore body → regex source):
 *   - `**` (between separators) → `.*`
 *   - `*`  → `[^/]*`  (does not cross /)
 *   - `?`  → `[^/]`
 *   - `[abc]` → preserved verbatim
 *   - `\X` → escaped literal X
 *   - other regex metacharacters are escaped
 *
 * Anchoring:
 *   - Pattern starts with `/` OR contains `/` (anywhere except trailing) →
 *     regex anchored at the rule's base
 *   - Otherwise → matches against any path component (regex prefixed
 *     with `(?:.*\/)?`)
 *
 * @version 2.1.4
 */

#include <entropic/mcp/servers/ignore_matcher.h>
#include <entropic/types/logging.h>

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
static auto logger = entropic::log::get("mcp.filesystem.ignore");

namespace entropic {

namespace {

/**
 * @brief Strip leading and trailing whitespace.
 * @param s Input string.
 * @return Trimmed copy.
 * @utility
 * @version 2.1.4
 */
std::string trim(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    auto end = s.find_last_not_of(" \t\r\n");
    return (begin == std::string::npos)
        ? std::string{}
        : s.substr(begin, end - begin + 1);
}

/**
 * @brief Convert filesystem path to forward-slash form.
 * @param p Path.
 * @return Forward-slash string. Empty if path is empty.
 * @utility
 * @version 2.1.4
 */
std::string to_slash(const fs::path& p) {
    auto s = p.generic_string();
    return s;
}

/**
 * @brief Test whether a regex character is a metacharacter we must
 *        escape when building a literal-in-regex segment.
 *
 * Excludes glob meta (`*`, `?`, `[`, `]`) which we handle separately.
 *
 * @param c Character.
 * @return true if must escape.
 * @utility
 * @version 2.1.4
 */
bool is_regex_meta(char c) {
    switch (c) {
    case '.': case '+': case '(': case ')':
    case '|': case '^': case '$': case '{':
    case '}': case '\\':
        return true;
    default:
        return false;
    }
}

} // namespace

// ── Pattern compilation ──────────────────────────────────

namespace {

/**
 * @brief Emit regex for a `*` or `**` glob token; advances `i` to the
 *        last character it consumed.
 *
 * Single `*` becomes `[^/]*` (does not cross /). `**` becomes `.*`
 * and consumes one trailing `/` if present (so `**` followed by a `/`
 * doesn't also produce a literal `/` in the output).
 *
 * @utility
 * @version 2.1.4
 */
void emit_star(const std::string& pattern, size_t& i, std::string& out) {
    bool double_star = (i + 1 < pattern.size())
        && pattern[i + 1] == '*';
    if (!double_star) { out += "[^/]*"; return; }
    out += ".*";
    ++i;
    if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
        ++i;
    }
}

/**
 * @brief Emit regex for a `[...]` bracket expression; advances `i` to
 *        the closing bracket.
 * @utility
 * @version 2.1.4
 */
void emit_bracket(const std::string& pattern, size_t& i,
                  std::string& out) {
    out += '[';
    ++i;
    while (i < pattern.size() && pattern[i] != ']') {
        out += pattern[i];
        ++i;
    }
    out += ']';
}

/**
 * @brief Emit regex for a backslash-escape; advances `i` past escape.
 * @utility
 * @version 2.1.4
 */
void emit_escape(const std::string& pattern, size_t& i,
                 std::string& out) {
    char next = pattern[i + 1];
    if (is_regex_meta(next)) { out += '\\'; }
    out += next;
    ++i;
}

} // namespace

/**
 * @brief Convert gitignore pattern body to regex source.
 *
 * Dispatch table: each glob token type has a small `emit_*` helper.
 *
 * @param pattern Pattern body (no leading `!` or trailing `/`).
 * @return Regex source.
 * @utility
 * @version 2.1.4
 */
namespace {

/**
 * @brief Emit regex for ONE pattern character; advances `i` for
 *        multi-char tokens (`**`, `[...]`, escapes).
 *
 * Flat dispatch — keeps the caller's nesting depth shallow.
 *
 * @utility
 * @version 2.1.4
 */
void emit_one(const std::string& pattern, size_t& i,
              std::string& out) {
    char c = pattern[i];
    bool handled = true;
    switch (c) {
    case '*': emit_star(pattern, i, out); break;
    case '?': out += "[^/]"; break;
    case '[': emit_bracket(pattern, i, out); break;
    case '\\':
        if (i + 1 < pattern.size()) {
            emit_escape(pattern, i, out);
        } else {
            handled = false;
        }
        break;
    default: handled = false; break;
    }
    if (!handled) {
        if (is_regex_meta(c)) { out += '\\'; }
        out += c;
    }
}

} // namespace

/**
 * @brief Convert a gitignore-style pattern body to a regex source.
 *
 * Walks character-by-character, dispatching to emit_one for each
 * token (which handles `*`, `**`, `?`, `[...]`, escapes, and
 * regex-meta literals).
 *
 * @param pattern Pattern body (no leading `!` or trailing `/`).
 * @return Regex source.
 * @internal
 * @version 2.1.4
 */
std::string IgnoreMatcher::pattern_to_regex(const std::string& pattern) {
    std::string out;
    out.reserve(pattern.size() * 2);
    for (size_t i = 0; i < pattern.size(); ++i) {
        emit_one(pattern, i, out);
    }
    return out;
}

/**
 * @brief Compile a gitignore line into a Rule.
 *
 * Strips leading `!` (negation) and trailing `/` (dir_only). Determines
 * anchor: pattern with embedded `/` (anywhere but at the end) is
 * anchored at the base; otherwise the pattern matches against any
 * path component.
 *
 * @internal
 * @version 2.1.4
 */
namespace {

/**
 * @brief Strip leading `!` (negation) and trailing `/` (dir_only) from
 *        a pattern body, recording flags on the rule.
 * @utility
 * @version 2.1.4
 */
void strip_flags(std::string& body, IgnoreMatcher::Rule& rule) {
    if (!body.empty() && body[0] == '!') {
        rule.negate = true;
        body.erase(0, 1);
    }
    if (!body.empty() && body.back() == '/') {
        rule.dir_only = true;
        body.pop_back();
    }
}

/**
 * @brief Build a regex prefix from a non-root anchor base.
 *
 * Escapes regex metacharacters in the base path (paths shouldn't
 * contain them but defensive). Returns empty when base is empty.
 *
 * @utility
 * @version 2.1.4
 */
std::string make_base_prefix(const std::string& base) {
    if (base.empty()) { return {}; }
    std::string raw = base + "/";
    std::string out;
    for (char c : raw) {
        if (is_regex_meta(c) || c == '*' || c == '?' || c == '[') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

/**
 * @brief Compile a regex source; return a never-match regex on
 *        std::regex_error, with a warning log line.
 * @utility
 * @version 2.1.4
 */
std::regex compile_or_never(const std::string& src,
                            const std::string& original_pattern) {
    try {
        return std::regex(src);
    } catch (const std::regex_error& e) {
        logger->warn("Skipping malformed ignore pattern '{}': {}",
                     original_pattern, e.what());
        return std::regex("(?!)");
    }
}

} // namespace

/**
 * @brief Compile a single gitignore line into a Rule.
 *
 * Strips leading `!` (negation) and trailing `/` (dir_only). Determines
 * anchor: pattern with embedded `/` (anywhere but at the end) or with
 * a leading `/` is anchored at the rule's base; otherwise the pattern
 * matches against any path component. Compiles BOTH re_exact (path is
 * the pattern target) and re_under (path is strictly below the target)
 * — together with dir_only this gives the correct match semantics for
 * gitignore directory patterns.
 *
 * @param pattern Gitignore line (already trimmed, non-comment).
 * @param base    Anchor base relative to root.
 * @return Compiled Rule.
 * @internal
 * @version 2.1.4
 */
IgnoreMatcher::Rule IgnoreMatcher::compile_pattern(
    const std::string& pattern, const std::string& base) {
    Rule rule;
    rule.original = pattern;
    rule.base = base;

    std::string body = pattern;
    strip_flags(body, rule);
    bool root_anchored = !body.empty() && body[0] == '/';
    if (root_anchored) { body.erase(0, 1); }
    bool anchored = root_anchored
        || body.find('/') != std::string::npos;

    std::string regex_body = pattern_to_regex(body);
    std::string base_prefix = make_base_prefix(base);
    std::string anchor_left = anchored
        ? ("^" + base_prefix)
        : ("^" + base_prefix + "(?:.*/)?");

    rule.re_exact = compile_or_never(
        anchor_left + regex_body + "$", pattern);
    rule.re_under = compile_or_never(
        anchor_left + regex_body + "/.*$", pattern);
    return rule;
}

// ── Public API ───────────────────────────────────────────

/**
 * @brief Add a pattern programmatically.
 * @internal
 * @version 2.1.4
 */
void IgnoreMatcher::add_pattern(const std::string& pattern,
                                const fs::path& base) {
    std::string trimmed = trim(pattern);
    if (trimmed.empty() || trimmed[0] == '#') { return; }
    rules_.push_back(compile_pattern(trimmed, to_slash(base)));
}

/**
 * @brief Load and parse one ignore file (gitignore or explorerignore).
 * @internal
 * @version 2.1.4
 */
void IgnoreMatcher::load_file(const fs::path& path,
                              const std::string& base) {
    std::ifstream in(path);
    if (!in.is_open()) { return; }
    std::string line;
    int loaded = 0;
    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') { continue; }
        rules_.push_back(compile_pattern(trimmed, base));
        ++loaded;
    }
    std::string base_label = base.empty() ? std::string("<root>") : base;
    logger->info("Loaded {} ignore rules from {} (base='{}')",
                 loaded, path.string(), base_label);
}

/**
 * @brief Load all .gitignore files (recursive) + .explorerignore.
 * @internal
 * @version 2.1.4
 */
void IgnoreMatcher::load(const fs::path& root) {
    rules_.clear();
    if (!fs::exists(root) || !fs::is_directory(root)) {
        logger->warn("IgnoreMatcher::load: root does not exist: {}",
                     root.string());
        return;
    }

    auto canonical_root = fs::weakly_canonical(root);
    fs::path root_gi = canonical_root / ".gitignore";
    if (fs::exists(root_gi)) { load_file(root_gi, ""); }

    // Recursively discover .gitignore files in subdirectories. We skip
    // the root one (already loaded) and also skip directories that the
    // accumulated rule set already excludes (avoids descending into
    // node_modules just to find an irrelevant .gitignore).
    try {
        auto it = fs::recursive_directory_iterator(
            canonical_root,
            fs::directory_options::skip_permission_denied);
        for (auto& entry : it) {
            if (!entry.is_regular_file()) { continue; }
            if (entry.path().filename() != ".gitignore") { continue; }
            if (entry.path() == root_gi) { continue; }
            auto rel_dir = fs::relative(entry.path().parent_path(),
                                        canonical_root);
            load_file(entry.path(), to_slash(rel_dir));
        }
    } catch (const std::exception& e) {
        logger->warn("Recursive gitignore scan aborted: {}", e.what());
    }

    fs::path explorer = canonical_root / ".explorerignore";
    if (fs::exists(explorer)) { load_file(explorer, ""); }
}

/**
 * @brief Test a path against the rule set (last-match-wins for negation).
 * @internal
 * @version 2.1.4
 */
bool IgnoreMatcher::is_ignored(const std::string& rel_path,
                               bool is_dir) const {
    bool ignored = false;
    for (const auto& rule : rules_) {
        bool match_under = std::regex_match(rel_path, rule.re_under);
        bool match_exact = std::regex_match(rel_path, rule.re_exact);
        // For dir_only rules, an exact match only counts when the path
        // is itself a directory (a regular file with the same name as
        // a `dir/` pattern is NOT excluded). re_under always counts —
        // any descendant inherits the parent's exclusion.
        bool exact_counts = match_exact
            && (!rule.dir_only || is_dir);
        if (match_under || exact_counts) {
            ignored = !rule.negate;
        }
    }
    return ignored;
}

} // namespace entropic
