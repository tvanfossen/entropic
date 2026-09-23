// SPDX-License-Identifier: Apache-2.0
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

/**
 * @brief Directory names never walked, by name alone (gh#161).
 *
 * `.git` alone is tens of thousands of entries on a large repository
 * and holds nothing a workspace search may return; the other three are
 * the same story for dependency and cache trees. Before gh#161 the
 * DISCOVERY walk honoured none of them — it walked the whole tree
 * looking for `.gitignore` files, contradicting its own comment — while
 * the glob/grep walk kept an identical list privately in
 * filesystem.cpp. One list, one owner.
 *
 * @param name Bare directory name.
 * @return true when the directory must never be descended into.
 * @req REQ-MCP-022
 * @version 2.13.0
 */
bool IgnoreMatcher::is_skipped_dir_name(const std::string& name) {
    static const std::vector<std::string> kSkipDirs = {
        ".git", "node_modules", "__pycache__", ".venv"
    };
    bool skipped = false;
    for (const auto& skip : kSkipDirs) {
        if (name == skip) { skipped = true; }
    }
    return skipped;
}

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
 * @return Regex source implementing the supported gitignore syntax —
 *         filename globs, `?`, character classes, double-star recursion
 *         — with regex metacharacters in literal text escaped.
 * @req REQ-MCP-022
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
 * @dg_internal
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
 * @brief Canonicalise an anchor base to the form a relative path
 *        prefix takes: no `./`, no trailing slash, "" for the root.
 *
 * The base string is now BOTH the regex prefix source and the key of
 * the bucket index (gh#161) — "sub", "sub/" and "./sub" have to reduce
 * to one spelling or a rule silently lands in a bucket nothing queries.
 *
 * @param base Raw base, relative to the workspace root.
 * @return Normalised base.
 * @utility
 * @version 2.13.0
 */
std::string normalize_base(const std::string& base) {
    std::string out = base;
    if (out.rfind("./", 0) == 0) { out.erase(0, 2); }
    if (out == ".") { out.clear(); }
    while (!out.empty() && out.back() == '/') { out.pop_back(); }
    return out;
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
 *
 * Malformed patterns and character classes are tolerated rather than
 * thrown — one bad line in a workspace .gitignore must not take down
 * glob/grep/read.
 *
 * @param src Regex source built from the pattern body.
 * @param original_pattern The gitignore line, for the warning log.
 * @return The compiled regex, or the never-matching `(?!)` regex when
 *         compilation failed.
 * @req REQ-MCP-022
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
 * @param base    Anchor base relative to root — each `.gitignore` is
 *                anchored at its own directory, matching git's
 *                documented behavior.
 * @return A compiled Rule carrying the negate/dir_only flags and both
 *         regexes, so a trailing-slash pattern matches the directory
 *         and its descendants but never a same-named file.
 * @req REQ-MCP-022
 * @version 2.13.0
 */
IgnoreMatcher::Rule IgnoreMatcher::compile_pattern(
    const std::string& pattern, const std::string& base) {
    Rule rule;
    rule.original = pattern;
    rule.base = normalize_base(base);

    std::string body = pattern;
    strip_flags(body, rule);
    bool root_anchored = !body.empty() && body[0] == '/';
    if (root_anchored) { body.erase(0, 1); }
    bool anchored = root_anchored
        || body.find('/') != std::string::npos;

    std::string regex_body = pattern_to_regex(body);
    std::string base_prefix = make_base_prefix(rule.base);
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
 * @brief Append a rule and index it under its anchor base (gh#161).
 *
 * The index stores POSITIONS in the ordered rule set, never copies, so
 * a bucketed query still sees the global source order — the property
 * last-match-wins depends on.
 *
 * @param rule Compiled rule; moved into the rule set.
 * @req REQ-MCP-022
 * @version 2.13.0
 */
void IgnoreMatcher::push_rule(Rule&& rule) {
    std::string base = rule.base;
    rules_.push_back(std::move(rule));
    by_base_[base].push_back(rules_.size() - 1);
}

/**
 * @brief Add a pattern programmatically.
 *
 * Comments and blank lines are skipped here as well as on the file
 * path, so a caller cannot inject a rule that matches everything.
 *
 * @param pattern Gitignore-syntax line.
 * @param base Directory the rule is anchored at, relative to the root.
 * @req REQ-MCP-022
 * @version 2.13.0
 */
void IgnoreMatcher::add_pattern(const std::string& pattern,
                                const fs::path& base) {
    std::string trimmed = trim(pattern);
    if (trimmed.empty() || trimmed[0] == '#') { return; }
    push_rule(compile_pattern(trimmed, to_slash(base)));
}

/**
 * @brief Load and parse one ignore file (gitignore or explorerignore).
 *
 * Comments and blank lines are skipped; every other line becomes a rule
 * anchored at `base`. A file that cannot be opened is a silent no-op.
 *
 * @param path Ignore file to read.
 * @param base Directory the file's rules are anchored at, relative to
 *             the workspace root ("" for the root file).
 * @req REQ-MCP-022
 * @version 2.13.0
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
        push_rule(compile_pattern(trimmed, base));
        ++loaded;
    }
    std::string base_label = base.empty() ? std::string("<root>") : base;
    logger->info("Loaded {} ignore rules from {} (base='{}')",
                 loaded, path.string(), base_label);
}

/**
 * @brief Load all .gitignore files (recursive) + .explorerignore.
 *
 * Clears any prior rules first, so load() is safe to call again when
 * the working directory changes. The root `.explorerignore` is layered
 * LAST, and matching is last-match-wins, so a workspace can both add
 * exclusions and re-include with `!`.
 *
 * @param root Workspace root to load from; a missing or non-directory
 *             root leaves the matcher empty and is logged.
 * @req REQ-MCP-022
 * @version 2.13.0
 */
void IgnoreMatcher::load(const fs::path& root) {
    rules_.clear();
    by_base_.clear();
    reset_regex_evals();
    if (!fs::exists(root) || !fs::is_directory(root)) {
        logger->warn("IgnoreMatcher::load: root does not exist: {}",
                     root.string());
        return;
    }

    auto canonical_root = fs::weakly_canonical(root);
    fs::path root_gi = canonical_root / ".gitignore";
    if (fs::exists(root_gi)) { load_file(root_gi, ""); }

    load_nested_gitignores(canonical_root, root_gi);

    fs::path explorer = canonical_root / ".explorerignore";
    if (fs::exists(explorer)) { load_file(explorer, ""); }
}

/**
 * @brief Recursively load nested .gitignore files under the root.
 * @param canonical_root Canonical project root.
 * @param root_gi The root .gitignore (skipped during the scan).
 * @req REQ-MCP-022
 * @version 2.13.0
 */
void IgnoreMatcher::load_nested_gitignores(
    const fs::path& canonical_root, const fs::path& root_gi) {
    try {
        fs::recursive_directory_iterator it(
            canonical_root,
            fs::directory_options::skip_permission_denied);
        const fs::recursive_directory_iterator end;
        for (; it != end; ++it) {
            visit_discovery_entry(it, canonical_root, root_gi);
        }
    } catch (const std::exception& e) {
        logger->warn("Recursive gitignore scan aborted: {}", e.what());
    }
}

/**
 * @brief Handle one entry of the discovery walk: prune or load.
 *
 * gh#161 made the pruning real. The pre-2.13.0 comment claimed the
 * scan skipped excluded directories; it skipped nothing, so a repo
 * vendoring boost/opencv/pcl walked every entry in the tree and loaded
 * ~200 vendored `.gitignore` files at startup — and again on every
 * `set_working_dir`.
 *
 * Two prunes, both matching git: a directory on the hardcoded skip
 * list is never entered, and neither is a directory the rules loaded
 * SO FAR already exclude — git cannot re-include anything below an
 * excluded directory, so a `.gitignore` down there is unreachable by
 * construction and reading it would be pure cost.
 *
 * @param[in,out] it Walk position; recursion is disabled on it to prune.
 * @param canonical_root Canonical project root.
 * @param root_gi The root `.gitignore`, already loaded.
 * @req REQ-MCP-022
 * @version 2.13.0
 */
void IgnoreMatcher::visit_discovery_entry(
    fs::recursive_directory_iterator& it,
    const fs::path& canonical_root, const fs::path& root_gi) {
    const auto& entry = *it;
    if (entry.is_directory()) {
        if (prunes_discovery(entry.path(), canonical_root)) {
            it.disable_recursion_pending();
        }
        return;
    }
    if (entry.path().filename() != ".gitignore"
        || entry.path() == root_gi) {
        return;
    }
    auto rel_dir = fs::relative(entry.path().parent_path(),
                                canonical_root);
    load_file(entry.path(), to_slash(rel_dir));
}

/**
 * @brief Whether the discovery walk must not descend into a directory.
 *
 * Two prunes, both matching git (gh#161): the hardcoded skip list, and
 * any directory the rules loaded SO FAR already exclude — git cannot
 * re-include anything below an excluded directory, so a `.gitignore`
 * down there is unreachable by construction.
 *
 * @param dir Directory being considered.
 * @param canonical_root Canonical project root.
 * @return true when the directory must not be descended into.
 * @req REQ-MCP-022
 * @version 2.13.0
 */
bool IgnoreMatcher::prunes_discovery(
    const fs::path& dir, const fs::path& canonical_root) const {
    if (is_skipped_dir_name(dir.filename().string())) { return true; }
    auto rel = to_slash(fs::relative(dir, canonical_root));
    return !rel.empty() && is_ignored(rel, true);
}

// ── Bucketed matching (gh#161) ───────────────────────────

namespace {

/// Index type mirroring IgnoreMatcher::by_base_.
using BaseIndex =
    std::unordered_map<std::string, std::vector<std::size_t>>;

/// Sentinel for "no rule left to examine".
constexpr std::size_t kNoRule = static_cast<std::size_t>(-1);

/**
 * @brief One anchor base's rule indices, consumed newest-first.
 * @dg_internal
 * @version 2.13.0
 */
struct BucketCursor {
    const std::vector<std::size_t>* indices; ///< Ascending indices
    std::size_t remaining;                   ///< Unexamined count
};

/**
 * @brief Gather the buckets that can possibly match `rel_path`.
 *
 * A rule anchored at base B compiles to `^B/…`, so it can only match a
 * path of which `B/` is a prefix. The candidate bases are therefore
 * exactly the root ("") plus every ancestor-directory prefix of the
 * path — the prefix ending at each `/`. Everything else in the rule
 * set is provably irrelevant and is never touched.
 *
 * @param index Base → rule indices.
 * @param rel_path Forward-slash path relative to the workspace root.
 * @param[out] out Cursors for the applicable buckets.
 * @utility
 * @version 2.13.0
 */
void collect_buckets(const BaseIndex& index, const std::string& rel_path,
                     std::vector<BucketCursor>& out) {
    auto take = [&](const std::string& base) {
        auto found = index.find(base);
        if (found != index.end() && !found->second.empty()) {
            out.push_back({&found->second, found->second.size()});
        }
    };
    take("");
    for (std::size_t i = 0; i < rel_path.size(); ++i) {
        if (rel_path[i] == '/') { take(rel_path.substr(0, i)); }
    }
}

/**
 * @brief Pop the highest unexamined rule index across all buckets.
 *
 * Descending global order is what lets the scan stop at the FIRST
 * match: last-match-wins means the last matching rule in source order
 * decides, so the first match found walking backwards IS the decision
 * and no rule before it can change the answer.
 *
 * @param cursors Bucket cursors, mutated in place.
 * @return The next rule index, or kNoRule when every bucket is spent.
 * @utility
 * @version 2.13.0
 */
std::size_t pop_highest(std::vector<BucketCursor>& cursors) {
    std::size_t best = kNoRule;
    BucketCursor* from = nullptr;
    for (auto& cursor : cursors) {
        if (cursor.remaining == 0) { continue; }
        std::size_t candidate = (*cursor.indices)[cursor.remaining - 1];
        if (from == nullptr || candidate > best) {
            best = candidate;
            from = &cursor;
        }
    }
    if (from != nullptr) { --from->remaining; }
    return best;
}

/**
 * @brief Test one rule against a path, counting the regexes it runs.
 *
 * `re_under` is tried first because a descendant match settles the
 * question on its own; `re_exact` is skipped entirely when a dir_only
 * rule meets a regular file, because an exact match could not have
 * counted anyway.
 *
 * @param rule Compiled rule.
 * @param rel_path Path relative to the workspace root.
 * @param is_dir Whether the path names a directory.
 * @param[in,out] evals Instrumentation counter (gh#161).
 * @return true when the rule matches the path.
 * @utility
 * @version 2.13.0
 */
bool rule_matches(const IgnoreMatcher::Rule& rule,
                  const std::string& rel_path, bool is_dir,
                  std::atomic<std::uint64_t>& evals) {
    evals.fetch_add(1, std::memory_order_relaxed);
    bool matched = std::regex_match(rel_path, rule.re_under);
    // For dir_only rules an exact match only counts when the path is
    // itself a directory — a regular file named like a `dir/` pattern
    // is NOT excluded.
    if (!matched && (!rule.dir_only || is_dir)) {
        evals.fetch_add(1, std::memory_order_relaxed);
        matched = std::regex_match(rel_path, rule.re_exact);
    }
    return matched;
}

} // namespace

/**
 * @brief Test a path against the rule set (last-match-wins for negation).
 *
 * Matching is path-relative, never filename-only, and negation stays
 * order-sensitive by construction. gh#161 changed HOW that order is
 * walked, never WHAT it means: the scan visits only the buckets on the
 * path's own ancestry, in descending global rule order, and stops at
 * the first match — which is by definition the last matching rule in
 * source order. Before, every rule in the workspace was evaluated
 * twice for every path: 5,061 rules against 187,855 files is the 87 s
 * glob the issue reported.
 *
 * @param rel_path Path relative to the workspace root, forward-slashed.
 * @param is_dir Whether the path names a directory — a `dir/` pattern
 *               matches the directory and its descendants but not a
 *               same-named regular file.
 * @return true when the LAST matching rule excludes the path; false
 *         when it re-includes it or nothing matched.
 * @req REQ-MCP-022
 * @version 2.13.0
 */
bool IgnoreMatcher::is_ignored(const std::string& rel_path,
                               bool is_dir) const {
    std::vector<BucketCursor> cursors;
    collect_buckets(by_base_, rel_path, cursors);

    bool ignored = false;
    for (std::size_t idx = pop_highest(cursors); idx != kNoRule;
         idx = pop_highest(cursors)) {
        if (rule_matches(rules_[idx], rel_path, is_dir, regex_evals_)) {
            ignored = !rules_[idx].negate;
            break;
        }
    }
    return ignored;
}

} // namespace entropic
