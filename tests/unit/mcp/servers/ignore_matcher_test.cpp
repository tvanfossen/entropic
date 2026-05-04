// SPDX-License-Identifier: LGPL-3.0-or-later
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
#include <filesystem>
#include <fstream>

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
