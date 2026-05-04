// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file ignore_matcher.h
 * @brief Path-relative ignore matching honoring .gitignore + .explorerignore.
 *
 * Issue #15 (v2.1.4): the filesystem MCP server pre-2.1.4 hardcoded a
 * tiny SKIP_DIRS list (`.git`, `node_modules`, `__pycache__`, `.venv`).
 * Anything else (build artifacts, doxygen output, generated XML, large
 * vendor blobs) leaked through grep/glob/read results, drowning real
 * matches in noise.
 *
 * IgnoreMatcher loads gitignore-style patterns from two sources:
 *
 *   1. `.gitignore` files — discovered recursively. Each file's rules
 *      are anchored at that file's directory (so `build/` in
 *      `<root>/sub/.gitignore` matches `<root>/sub/build/...` only,
 *      matching git's documented behavior).
 *   2. `.explorerignore` (workspace root only) — supplementary, applied
 *      after gitignore (so it can both add and re-include via `!`).
 *
 * Supported gitignore syntax:
 *   - Filename globs: `\*.log`, `?.tmp`, `[abc].py`
 *   - Directory match: `build/` (trailing slash means directory only)
 *   - Path-anchored: `/foo` (root-anchored), `foo/bar` (path contains /)
 *   - Recursive: leading double-star matches any depth; trailing
 *     double-star is recursive descent
 *   - Negation: `!keep.log` (re-include after a broader exclude)
 *   - Comments: `# ...`
 *   - Blank lines skipped
 *
 * Matching is path-relative to `root` (forward-slash form), never
 * filename-only — `build/foo.o` correctly matches `build/`.
 *
 * @version 2.1.4
 */

#pragma once

#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief gitignore-style path matcher (#15, v2.1.4).
 *
 * Single instance owns a compiled rule set. `is_ignored` is O(rules)
 * per call; rules are evaluated in source order, last-match-wins so
 * that negation patterns (`!keep.log`) can re-include paths that
 * earlier patterns excluded.
 *
 * @version 2.1.4
 */
class IgnoreMatcher {
public:
    /**
     * @brief Construct an empty matcher (no rules).
     * @version 2.1.4
     */
    IgnoreMatcher() = default;

    /**
     * @brief Load gitignore + explorerignore from a workspace root.
     *
     * Walks `root` recursively for `.gitignore` files and loads them
     * with their per-directory anchoring. Then loads
     * `<root>/.explorerignore` (if present) anchored at root. Existing
     * rules are cleared first.
     *
     * Safe to call repeatedly when the working directory changes.
     *
     * @param root Workspace root directory (must exist; non-canonical
     *             paths are accepted).
     * @version 2.1.4
     */
    void load(const std::filesystem::path& root);

    /**
     * @brief Add a single pattern programmatically (test surface).
     *
     * @param pattern Gitignore-syntax pattern.
     * @param base    Anchor directory (relative to root). Empty means
     *                "anchored at root."
     * @version 2.1.4
     */
    void add_pattern(const std::string& pattern,
                     const std::filesystem::path& base = {});

    /**
     * @brief Test whether a path is ignored.
     *
     * @param rel_path Forward-slash, relative-to-root path. Must NOT
     *                 start with `/`. Trailing slash optional.
     * @param is_dir   True if the path refers to a directory.
     * @return true if the path matches an active (non-negated)
     *         exclusion rule.
     * @version 2.1.4
     */
    bool is_ignored(const std::string& rel_path,
                    bool is_dir) const;

    /**
     * @brief Number of compiled rules (test surface).
     * @return Rule count.
     * @utility
     * @version 2.1.4
     */
    std::size_t rule_count() const { return rules_.size(); }

    /**
     * @brief One compiled gitignore rule (public so internal helpers
     *        in the .cpp can construct Rules without friend declarations).
     *
     * Two regexes per rule disambiguate the dir_only semantic:
     *   - re_exact matches the path IS the pattern (no descendants)
     *   - re_under matches the path is STRICTLY BELOW the pattern
     *
     * For dir_only patterns (`build/`), re_exact only counts when the
     * path is itself a directory; re_under always counts (descendants).
     * For regular patterns (`*.log`, `foo`), either regex counts.
     *
     * @version 2.1.4
     */
    struct Rule {
        std::regex re_exact;      ///< Path equals pattern target
        std::regex re_under;      ///< Path is strictly below target
        bool negate = false;      ///< Leading `!`
        bool dir_only = false;    ///< Trailing `/` (directory match)
        std::string original;     ///< Raw pattern for diagnostics
        std::string base;         ///< Anchor base, "" means root
    };

private:

    /**
     * @brief Compile a single pattern into a Rule.
     * @param pattern Gitignore pattern (already trimmed, non-comment).
     * @param base    Anchor base relative to root.
     * @return Compiled Rule.
     * @utility
     * @version 2.1.4
     */
    static Rule compile_pattern(const std::string& pattern,
                                const std::string& base);

    /**
     * @brief Convert a gitignore pattern body to a POSIX regex source.
     *
     * Handles `*`, `**`, `?`, `[...]`, escape via `\`. Caller anchors
     * the result with `^` / `$` and any base prefix.
     *
     * @param pattern Pattern body (no leading `!` or trailing `/`).
     * @return Regex source string.
     * @utility
     * @version 2.1.4
     */
    static std::string pattern_to_regex(const std::string& pattern);

    /**
     * @brief Load and parse a single ignore file.
     * @param path Path to the file (e.g. <root>/sub/.gitignore).
     * @param base Anchor base relative to root (forward slash).
     * @utility
     * @version 2.1.4
     */
    void load_file(const std::filesystem::path& path,
                   const std::string& base);

    std::vector<Rule> rules_;     ///< Rule set (source order)
};

} // namespace entropic
