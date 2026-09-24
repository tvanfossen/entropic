// SPDX-License-Identifier: Apache-2.0
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

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief gitignore-style path matcher (#15, v2.1.4).
 *
 * Single instance owns a compiled rule set, ordered, with
 * last-match-wins semantics so that negation patterns (`!keep.log`)
 * can re-include paths that earlier patterns excluded.
 *
 * `is_ignored` was O(all rules) per call until gh#161 (v2.13.0): on a
 * repository vendoring boost/opencv/pcl that meant 5,061 rules x 2
 * regexes for every path, and one recursive glob over header files
 * took 87 seconds.
 * It is now O(rules anchored on the path's own ancestry), because a
 * rule loaded from `deps/boost/libs/.gitignore` cannot match anything
 * outside `deps/boost/libs/`, and it stops at the first match found
 * walking that subset BACKWARDS — which is the last match in source
 * order, so the semantics above are unchanged.
 *
 * @version 2.13.0
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
     * The discovery walk PRUNES (gh#161): it never descends into a
     * skip-list directory, nor into one the rules loaded so far
     * already exclude — matching git, which cannot re-include anything
     * below an excluded directory. Note the ordering consequence:
     * `.explorerignore` is layered last so it can override gitignore,
     * which also means its exclusions are not yet known during
     * discovery and therefore do not prune it.
     *
     * Safe to call repeatedly when the working directory changes.
     *
     * @param root Workspace root directory (must exist; non-canonical
     *             paths are accepted).
     * @version 2.13.0
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
     * @brief Regex evaluations performed since the last reset (gh#161).
     *
     * Deterministic instrumentation: every `std::regex_match` this
     * matcher runs increments it exactly once, so a test can assert
     * WORK DONE rather than wall-clock time (which is machine-load
     * dependent and therefore untestable). A relaxed atomic add next
     * to a regex match is free in any measurable sense, so the counter
     * is compiled in unconditionally rather than behind a test macro
     * that would let the instrumented and shipped paths diverge.
     *
     * @return Cumulative regex evaluations.
     * @utility
     * @version 2.13.0
     */
    std::uint64_t regex_evals() const {
        return regex_evals_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Reset the regex-evaluation counter (gh#161, test surface).
     * @utility
     * @version 2.13.0
     */
    void reset_regex_evals() const {
        regex_evals_.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Directory names never walked, by name alone (gh#161).
     *
     * Single source of truth for the hardcoded skip list: the
     * `.gitignore` DISCOVERY walk here and the glob/grep walk in
     * filesystem.cpp both consult it, so the two cannot drift. These
     * directories hold no rules a workspace search should honour and
     * are the largest entry counts in a typical tree.
     *
     * @param name Bare directory name (no separators).
     * @return true when the directory must never be descended into.
     * @utility
     * @version 2.13.0
     */
    static bool is_skipped_dir_name(const std::string& name);

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

    /**
     * @brief Recursively load nested .gitignore files under the root.
     *
     * Extracted from load() to keep it knots-clean. Skips the already-
     * loaded root .gitignore, never descends into `is_skipped_dir_name`
     * directories, and never descends into a directory the rules loaded
     * SO FAR already exclude (gh#161).
     *
     * @param canonical_root Canonical project root.
     * @param root_gi The root .gitignore (skipped during the scan).
     * @dg_internal
     * @version 2.13.0
     */
    void load_nested_gitignores(const std::filesystem::path& canonical_root,
                                const std::filesystem::path& root_gi);

    /**
     * @brief Handle one entry of the discovery walk: prune or load.
     *
     * Pruning is done by disabling recursion on the caller's iterator,
     * which is why the iterator is passed by reference (gh#161).
     *
     * @param[in,out] it Current walk position.
     * @param canonical_root Canonical project root.
     * @param root_gi The root `.gitignore` (already loaded).
     * @dg_internal
     * @version 2.13.0
     */
    void visit_discovery_entry(
        std::filesystem::recursive_directory_iterator& it,
        const std::filesystem::path& canonical_root,
        const std::filesystem::path& root_gi);

    /**
     * @brief Whether the discovery walk must not descend into a
     *        directory (skip list, or already excluded) — gh#161.
     *
     * @param dir Directory being considered.
     * @param canonical_root Canonical project root.
     * @return true when the directory must be pruned.
     * @dg_internal
     * @version 2.13.0
     */
    bool prunes_discovery(
        const std::filesystem::path& dir,
        const std::filesystem::path& canonical_root) const;

    /**
     * @brief Append a compiled rule and index it by its anchor base.
     *
     * The ONLY place `rules_` grows, so the base index can never fall
     * out of step with the ordered rule set (gh#161).
     *
     * @param rule Compiled rule; consumed.
     * @dg_internal
     * @version 2.13.0
     */
    void push_rule(Rule&& rule);

    std::vector<Rule> rules_;     ///< Rule set (source order)

    /// Anchor base → indices into `rules_`, ascending. A rule anchored
    /// at `base` can only ever match paths under `base/`, so a query
    /// consults the buckets on its own ancestry and nothing else
    /// (gh#161). Indices keep the global source order, which is what
    /// makes last-match-wins survive the bucketing.
    std::unordered_map<std::string, std::vector<std::size_t>> by_base_;

    /// Regex evaluations performed (gh#161 instrumentation).
    mutable std::atomic<std::uint64_t> regex_evals_{0};
};

} // namespace entropic
