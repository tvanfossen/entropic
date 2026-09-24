// SPDX-License-Identifier: Apache-2.0
/**
 * @file ignore_matcher_test.cpp
 * @brief Unit tests for IgnoreMatcher (#15, v2.1.4).
 *
 * Tests pattern compilation and path matching in isolation from the
 * filesystem server, plus integration with on-disk .gitignore /
 * .explorerignore file loading.
 *
 * @version 2.1.4
 */

#include <entropic/mcp/servers/ignore_matcher.h>

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace entropic;

// ── RAII temp directory ──────────────────────────────────

namespace {

/**
 * @brief Create a unique temp directory; remove on destruction.
 * @internal
 * @version 2.1.4
 */
class TempDir {
public:
    TempDir() {
        // PID-qualified to avoid /tmp collisions across CTest's
        // parallel SCENARIO processes.
        static std::atomic<int> instance_counter{0};
        auto base = fs::temp_directory_path() / "entropic_ignore_test";
        int idx = instance_counter.fetch_add(1);
        path_ = base.string() + "_p"
              + std::to_string(static_cast<long>(::getpid()))
              + "_i" + std::to_string(idx);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }
private:
    fs::path path_;
};

/**
 * @brief Write a string to a file, creating parent directories.
 * @internal
 * @version 2.1.4
 */
void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << content;
}

} // namespace

// ── Pattern: filename glob ───────────────────────────────

TEST_CASE("IgnoreMatcher: filename glob matches at any depth",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    IgnoreMatcher m;
    m.add_pattern("*.log");
    CHECK(m.is_ignored("x.log", false));
    CHECK(m.is_ignored("dir/x.log", false));
    CHECK(m.is_ignored("a/b/c/x.log", false));
    CHECK_FALSE(m.is_ignored("x.txt", false));
}

TEST_CASE("IgnoreMatcher: directory pattern matches dir + descendants",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    IgnoreMatcher m;
    m.add_pattern("build/");
    CHECK(m.is_ignored("build", true));
    CHECK(m.is_ignored("build/foo.o", false));
    CHECK(m.is_ignored("build/x/y/z.o", false));
    CHECK_FALSE(m.is_ignored("src/build.cpp", false)); // not under build/
}

TEST_CASE("IgnoreMatcher: trailing slash means dir-only — does NOT "
          "match a regular file with the same name",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    IgnoreMatcher m;
    m.add_pattern("build/");
    CHECK_FALSE(m.is_ignored("build", false)); // file named "build"
    CHECK(m.is_ignored("build", true));         // directory named "build"
}

TEST_CASE("IgnoreMatcher: leading-slash anchors at root",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    IgnoreMatcher m;
    m.add_pattern("/foo");
    CHECK(m.is_ignored("foo", false));
    CHECK_FALSE(m.is_ignored("sub/foo", false));
}

TEST_CASE("IgnoreMatcher: pattern with embedded slash is anchored",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    IgnoreMatcher m;
    m.add_pattern("docs/doxygen/");
    CHECK(m.is_ignored("docs/doxygen", true));
    CHECK(m.is_ignored("docs/doxygen/index.html", false));
    CHECK_FALSE(m.is_ignored("docs/other.md", false));
}

TEST_CASE("IgnoreMatcher: double-star matches any depth",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    IgnoreMatcher m;
    m.add_pattern("**/cache");
    CHECK(m.is_ignored("cache", true));
    CHECK(m.is_ignored("a/cache", true));
    CHECK(m.is_ignored("a/b/c/cache", true));
}

TEST_CASE("IgnoreMatcher: negation re-includes after broader exclude",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    IgnoreMatcher m;
    m.add_pattern("*.log");
    m.add_pattern("!keep.log");
    CHECK(m.is_ignored("error.log", false));
    CHECK_FALSE(m.is_ignored("keep.log", false));
}

TEST_CASE("IgnoreMatcher: negation order matters (last-match-wins)",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    IgnoreMatcher m;
    // First !keep.log (no-op since nothing excluded yet),
    // then *.log (which DOES exclude keep.log because *.log fires
    // last). Last-match-wins → keep.log stays excluded.
    m.add_pattern("!keep.log");
    m.add_pattern("*.log");
    CHECK(m.is_ignored("keep.log", false));
}

TEST_CASE("IgnoreMatcher: comments and blank lines skipped during "
          "load",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    IgnoreMatcher m;
    m.add_pattern("# this is a comment");
    m.add_pattern("");
    m.add_pattern("   ");
    CHECK(m.rule_count() == 0);

    m.add_pattern("real_pattern");
    CHECK(m.rule_count() == 1);
}

// ── Bracket character class ──────────────────────────────

TEST_CASE("IgnoreMatcher: bracket character class",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    IgnoreMatcher m;
    m.add_pattern("[abc].txt");
    CHECK(m.is_ignored("a.txt", false));
    CHECK(m.is_ignored("b.txt", false));
    CHECK_FALSE(m.is_ignored("d.txt", false));
}

// ── On-disk loading ──────────────────────────────────────

TEST_CASE("IgnoreMatcher::load reads root .gitignore",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    TempDir tmp;
    write_file(tmp.path() / ".gitignore",
               "# comment\nbuild/\n*.log\n");

    IgnoreMatcher m;
    m.load(tmp.path());

    CHECK(m.is_ignored("build/foo.o", false));
    CHECK(m.is_ignored("err.log", false));
    CHECK_FALSE(m.is_ignored("src/main.cpp", false));
}

TEST_CASE("IgnoreMatcher::load layers .explorerignore over .gitignore",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    TempDir tmp;
    write_file(tmp.path() / ".gitignore", "build/\n");
    write_file(tmp.path() / ".explorerignore", "*.xml\n");

    IgnoreMatcher m;
    m.load(tmp.path());

    CHECK(m.is_ignored("build/x.o", false));
    CHECK(m.is_ignored("data.xml", false));
    CHECK_FALSE(m.is_ignored("src/main.cpp", false));
}

TEST_CASE("IgnoreMatcher::load discovers nested .gitignore "
          "anchored at its directory",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    TempDir tmp;
    // root .gitignore excludes nothing useful here.
    write_file(tmp.path() / ".gitignore", "");
    // sub/.gitignore excludes only paths INSIDE sub/.
    write_file(tmp.path() / "sub" / ".gitignore", "ignore.txt\n");
    write_file(tmp.path() / "sub" / "ignore.txt", "x");
    write_file(tmp.path() / "sub" / "keep.txt", "y");
    write_file(tmp.path() / "ignore.txt", "z"); // root

    IgnoreMatcher m;
    m.load(tmp.path());

    CHECK(m.is_ignored("sub/ignore.txt", false));
    CHECK_FALSE(m.is_ignored("sub/keep.txt", false));
    // Root-level ignore.txt is NOT excluded — sub/.gitignore only
    // applies under sub/.
    CHECK_FALSE(m.is_ignored("ignore.txt", false));
}

TEST_CASE("IgnoreMatcher::load is idempotent; re-load clears "
          "previous rules",
          "[mcp][ignore_matcher][2.1.4][issue-15]") {
    TempDir tmp;
    write_file(tmp.path() / ".gitignore", "*.log\n");

    IgnoreMatcher m;
    m.load(tmp.path());
    auto first_count = m.rule_count();
    REQUIRE(first_count >= 1);

    m.load(tmp.path()); // second call should not double-count.
    REQUIRE(m.rule_count() == first_count);
}

// ── v2.3.10: coverage for emit_escape, '?' glob, and malformed regex ──

TEST_CASE("IgnoreMatcher handles '?' single-char glob",
          "[mcp][ignore_matcher][v2.3.10][coverage]") {
    TempDir tmp;
    // emit_one's '?' branch → out += "[^/]"
    write_file(tmp.path() / ".gitignore", "file?.tmp\n");
    IgnoreMatcher m;
    m.load(tmp.path());

    CHECK(m.is_ignored("file1.tmp", false));
    CHECK(m.is_ignored("fileX.tmp", false));
    // '?' does not match '/'
    CHECK_FALSE(m.is_ignored("file/.tmp", false));
    // '?' matches exactly one char
    CHECK_FALSE(m.is_ignored("file12.tmp", false));
}

TEST_CASE("IgnoreMatcher compiles patterns containing backslash escapes",
          "[mcp][ignore_matcher][v2.3.10][coverage]") {
    TempDir tmp;
    // emit_escape branch (lines 139-145 in ignore_matcher.cpp): the
    // pattern compiler sees `\` and consumes the next char as a
    // literal. Different shells / git versions disagree on the exact
    // surface behavior; the coverage assertion is just that load()
    // accepts the pattern without throwing and the rule registers.
    write_file(tmp.path() / ".gitignore", "\\*.txt\n");
    IgnoreMatcher m;
    m.load(tmp.path());

    REQUIRE(m.rule_count() >= 1);
    // A "normal" filename without a literal `*` must not match —
    // the escape kept `*` as literal rather than glob-wildcard.
    CHECK_FALSE(m.is_ignored("normal.txt", false));
}

// ── gh#161: bounded work, pruned discovery ───────────────

namespace {

/**
 * @brief Build a wide tree of nested .gitignore files.
 *
 * `tops` x `subs` directories, each with its own `.gitignore` holding
 * `rules` patterns — the shape of a repository that vendors its
 * dependencies, where the reporter measured 5,061 rules.
 *
 * @internal
 * @version 2.13.0
 */
void build_nested_ignore_tree(const fs::path& root, int tops, int subs,
                              int rules) {
    for (int t = 0; t < tops; ++t) {
        auto top = root / ("d" + std::to_string(t));
        for (int s = 0; s < subs; ++s) {
            auto dir = top / ("s" + std::to_string(s));
            std::string body;
            for (int r = 0; r < rules; ++r) {
                body += "d" + std::to_string(t) + "_s"
                      + std::to_string(s) + "_r" + std::to_string(r)
                      + ".dat\n";
            }
            write_file(dir / ".gitignore", body);
            write_file(dir / "probe.cpp", "x");
        }
        write_file(top / ".gitignore",
                   "top" + std::to_string(t) + ".out\n");
    }
}

} // namespace

TEST_CASE("IgnoreMatcher evaluates only the rules on a path's own "
          "ancestry",
          "[mcp][ignore_matcher][2.13.0][gh-161]") {
    // gh#161: a repo vendoring boost/opencv/pcl loaded 5,061 rules and
    // is_ignored ran TWO regexes against EVERY one of them for EVERY
    // path — 87 s for a single glob. A rule anchored at
    // deps/boost/libs/geometry/doc cannot match src/main.cpp, so the
    // only defensible bound on the work is the rules on the path's own
    // ancestry. Asserted as an evaluation COUNT, never as elapsed time.
    TempDir tmp;
    write_file(tmp.path() / ".gitignore", "*.tmp\n*.bak\n");
    constexpr int kTops = 8;
    constexpr int kSubs = 8;
    constexpr int kRules = 10;
    build_nested_ignore_tree(tmp.path(), kTops, kSubs, kRules);

    IgnoreMatcher m;
    m.load(tmp.path());

    // 2 root + 8 top-level + 640 leaf rules across 73 distinct bases.
    REQUIRE(m.rule_count() >= 640);

    // Ancestry of d3/s5/probe.cpp: root (2) + d3 (1) + d3/s5 (10) = 13
    // rules, so 26 regex evaluations is the ceiling — two per rule,
    // and the implementation may legitimately use fewer.
    constexpr std::uint64_t kAncestryCeiling = 2 * (2 + 1 + 10);

    m.reset_regex_evals();
    CHECK_FALSE(m.is_ignored("d3/s5/probe.cpp", false));
    CHECK(m.regex_evals() <= kAncestryCeiling);

    // Same bound when the path IS ignored, by its own directory's rule…
    m.reset_regex_evals();
    CHECK(m.is_ignored("d3/s5/d3_s5_r4.dat", false));
    CHECK(m.regex_evals() <= kAncestryCeiling);

    // …and when it is ignored by a rule from the ROOT .gitignore.
    m.reset_regex_evals();
    CHECK(m.is_ignored("d3/s5/scratch.tmp", false));
    CHECK(m.regex_evals() <= kAncestryCeiling);

    // A sibling's rule must not leak across bases.
    CHECK_FALSE(m.is_ignored("d3/s5/d2_s1_r0.dat", false));
}

TEST_CASE("IgnoreMatcher discovery prunes skip-list and ignored "
          "directories",
          "[mcp][ignore_matcher][2.13.0][gh-161]") {
    // gh#161: load_nested_gitignores claimed in its own comment to skip
    // excluded directories and did not — it walked the ENTIRE tree,
    // .git included, loading ~200 vendored .gitignore files at startup.
    // Rule count is the probe: a .gitignore inside a pruned directory
    // was never opened, so its rules cannot be in the set.
    TempDir tmp;
    write_file(tmp.path() / ".gitignore", "vendor/\n");
    write_file(tmp.path() / ".git" / "hooks" / ".gitignore", "a\nb\nc\n");
    write_file(tmp.path() / "node_modules" / "pkg" / ".gitignore", "d\ne\n");
    write_file(tmp.path() / "__pycache__" / ".gitignore", "f\n");
    write_file(tmp.path() / ".venv" / "lib" / ".gitignore", "g\n");
    write_file(tmp.path() / "vendor" / "boost" / ".gitignore", "h\ni\n");
    write_file(tmp.path() / "src" / ".gitignore", "j\n");

    IgnoreMatcher m;
    m.load(tmp.path());

    // Root `vendor/` + src's `j`. Nothing else was even opened.
    CHECK(m.rule_count() == 2);
    CHECK(m.is_ignored("src/j", false));
    CHECK(m.is_ignored("vendor/boost/x.hpp", false));
    CHECK_FALSE(m.is_ignored(".git/hooks/a", false));
    CHECK_FALSE(m.is_ignored("node_modules/pkg/d", false));
    CHECK_FALSE(m.is_ignored("__pycache__/f", false));
    CHECK_FALSE(m.is_ignored(".venv/lib/g", false));
    // git itself never re-includes below an excluded directory, so a
    // .gitignore under vendor/ is unreachable by construction — the
    // path stays excluded by the root's `vendor/` rule either way.
    CHECK(m.is_ignored("vendor/boost/h", false));
}

TEST_CASE("IgnoreMatcher preserves last-match-wins ACROSS anchor bases",
          "[mcp][ignore_matcher][2.13.0][gh-161]") {
    // The semantic guard on gh#161's bucketing: consulting only the
    // buckets on a path's ancestry must not reorder the rule set. A
    // naive "deepest base first" bucketing passes every other test in
    // this file and fails this one.
    TempDir tmp;
    write_file(tmp.path() / ".gitignore", "");
    // Loaded second, base "sub": exclude *.txt, re-include early.txt.
    write_file(tmp.path() / "sub" / ".gitignore", "*.txt\n!early.txt\n");
    // Loaded LAST, base "" (the parent): re-include sub/keep.txt and
    // re-exclude sub/early.txt. Both are later rules at a SHALLOWER
    // base than the ones they override.
    write_file(tmp.path() / ".explorerignore",
               "!sub/keep.txt\nsub/early.txt\n");

    IgnoreMatcher m;
    m.load(tmp.path());

    CHECK(m.is_ignored("sub/other.txt", false));   // nested rule stands
    CHECK_FALSE(m.is_ignored("sub/keep.txt", false));  // later negation
    CHECK(m.is_ignored("sub/early.txt", false));       // later exclude
}

TEST_CASE("IgnoreMatcher tolerates a malformed character class",
          "[mcp][ignore_matcher][v2.3.10][coverage][failure-mode]") {
    TempDir tmp;
    // An unclosed character class `[abc` would produce a regex syntax
    // error if not handled defensively. compile_or_never's catch path
    // (lines 278-282 in src/mcp/servers/ignore_matcher.cpp) substitutes
    // a never-match regex `(?!)`. Concrete shape: a `[` without `]`
    // emits a backslash-escaped character class that std::regex rejects
    // on some platforms — the compile-fallback must keep load() from
    // throwing.
    write_file(tmp.path() / ".gitignore",
               "[unclosed\nvalid.log\n");
    IgnoreMatcher m;
    m.load(tmp.path());

    // The valid pattern still applies even if the malformed one is dropped.
    CHECK(m.is_ignored("valid.log", false));
    // The malformed pattern matches nothing.
    CHECK_FALSE(m.is_ignored("unclosed", false));
}
