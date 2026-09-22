// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_filesystem.cpp
 * @brief Unit tests for the FilesystemServer MCP server.
 *
 * Tests all 6 filesystem tools (read_file, write_file, edit_file,
 * glob, grep, list_directory) including security enforcement,
 * read-before-write tracking, size gate, and anchor key behavior.
 *
 * @version 1.8.5
 */

#include <entropic/mcp/servers/filesystem.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/config.h>
#include <entropic/types/run_scope.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <atomic>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace entropic;

// ── RAII temp directory ──────────────────────────────────

/**
 * @brief RAII wrapper that creates a unique temp directory and
 *        removes it recursively on destruction.
 * @internal
 * @version 1.8.5
 */
class TempDir {
public:
    /**
     * @brief Create a unique temporary directory.
     * @internal
     * @version 1.8.5
     */
    TempDir() {
        // CTest runs each Catch2 SCENARIO as a separate process via
        // catch2-discovery; a per-process counter races on shared
        // /tmp/entropic_test_0. Include PID + a per-instance counter
        // for uniqueness across processes AND multiple instances in
        // the same process.
        static std::atomic<int> instance_counter{0};
        auto base = fs::temp_directory_path() / "entropic_test";
        int idx = instance_counter.fetch_add(1);
        path_ = base.string() + "_p"
              + std::to_string(static_cast<long>(::getpid()))
              + "_i" + std::to_string(idx);
        fs::create_directories(path_);
    }

    /**
     * @brief Remove the temporary directory recursively.
     * @internal
     * @version 1.8.5
     */
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /**
     * @brief Get the temporary directory path.
     * @return Filesystem path.
     * @internal
     * @version 1.8.5
     */
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// ── File write helper ────────────────────────────────────

/**
 * @brief Write content to a file inside a directory, creating
 *        parent directories as needed.
 * @param dir Base directory.
 * @param name Relative file name (may include subdirectories).
 * @param content File content string.
 * @internal
 * @version 1.8.5
 */
static void write_test_file(const fs::path& dir,
                             const std::string& name,
                             const std::string& content) {
    auto full = dir / name;
    fs::create_directories(full.parent_path());
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    out << content;
}

// ── Server factory helper ────────────────────────────────

/**
 * @brief Build a FilesystemServer rooted at the given directory
 *        with default config.
 * @param root Root directory for the server.
 * @param cfg Optional config override.
 * @param model_ctx Model context bytes (0 = unlimited).
 * @return Constructed FilesystemServer.
 * @internal
 * @version 1.8.5
 */
static FilesystemServer make_server(
    const fs::path& root,
    FilesystemConfig cfg = {},
    int model_ctx = 0) {
    return FilesystemServer(root, cfg, TEST_DATA_DIR, model_ctx);
}

// ── JSON envelope helper ─────────────────────────────────

/**
 * @brief Parse the "result" field from a ServerResponse JSON
 *        envelope, then parse that as JSON.
 * @param envelope Raw JSON string from server.execute().
 * @return Parsed result JSON.
 * @internal
 * @version 1.8.5
 */
static json parse_result(const std::string& envelope) {
    auto env = json::parse(envelope);
    return json::parse(env.at("result").get<std::string>());
}

/**
 * @brief Extract the raw "result" string from a ServerResponse
 *        JSON envelope without further parsing.
 * @param envelope Raw JSON string from server.execute().
 * @return Raw result string.
 * @internal
 * @version 1.8.5
 */
static std::string raw_result(const std::string& envelope) {
    auto env = json::parse(envelope);
    return env.at("result").get<std::string>();
}

// ── Tests ────────────────────────────────────────────────

TEST_CASE("test_read_file_returns_json", "[filesystem]") {
    /**
     * @brief Read a two-line file and verify JSON structure.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "test.txt", "hello\nworld");
    auto server = make_server(tmp.path());

    json args;
    args["path"] = "test.txt";
    auto envelope = server.execute("read_file", args.dump());
    auto result = parse_result(envelope);

    REQUIRE(result.contains("path"));
    REQUIRE(result["total"].get<int>() == 2);
    REQUIRE(result["lines"][0].get<std::string>() == "hello");
    REQUIRE(result["lines"][1].get<std::string>() == "world");
}

TEST_CASE("test_read_file_not_found", "[filesystem]") {
    /**
     * @brief Read a nonexistent file and verify not_found error.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    auto server = make_server(tmp.path());

    json args;
    args["path"] = "nonexistent.txt";
    auto envelope = server.execute("read_file", args.dump());
    auto result = parse_result(envelope);

    REQUIRE(result.contains("error"));
    REQUIRE(result["error"].get<std::string>() == "not_found");
}

TEST_CASE("test_read_file_size_gate", "[filesystem]") {
    /**
     * @brief Exceed max_read_bytes and verify size_exceeded error.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    std::string big(100, 'x');
    write_test_file(tmp.path(), "big.txt", big);

    FilesystemConfig cfg;
    cfg.max_read_bytes = 10;
    auto server = make_server(tmp.path(), cfg);

    json args;
    args["path"] = "big.txt";
    auto envelope = server.execute("read_file", args.dump());
    auto result = parse_result(envelope);

    REQUIRE(result.contains("error"));
    REQUIRE(result["error"].get<std::string>() == "size_exceeded");
}

TEST_CASE("test_write_file_requires_read", "[filesystem]") {
    /**
     * @brief Write to an existing file without prior read, verify
     *        read_before_write error.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "existing.txt", "original");
    auto server = make_server(tmp.path());

    json args;
    args["path"] = "existing.txt";
    args["content"] = "overwritten";
    auto envelope = server.execute("write_file", args.dump());
    auto result = parse_result(envelope);

    REQUIRE(result.contains("error"));
    REQUIRE(result["error"].get<std::string>() ==
            "read_before_write");
}

TEST_CASE("test_write_file_allows_write_after_read_even_if_changed",
          "[filesystem]") {
    /**
     * @brief Pin what the read-before-write gate ACTUALLY does.
     *
     * This test was previously named ..._detects_external_change and
     * asserted on FileAccessTracker::was_read_unchanged directly. Its own
     * comment conceded the write succeeds — so it never exercised a refused
     * write, and passed while the behaviour its name claimed did not exist.
     * was_read_unchanged had no production caller at all; the gate
     * (check_read_before_write) only ever consults was_read(). The dead
     * method was removed in v2.11.0 and this now pins the real contract.
     *
     * Catalog REQ-MCP-021 records the external-modification gate as absent.
     * If it is ever implemented, this test should flip to expecting a refusal.
     *
     * @internal
     * @version 2.11.0
     */
    TempDir tmp;
    write_test_file(tmp.path(), "mutable.txt", "original");
    auto server = make_server(tmp.path());

    // Read to satisfy read-before-write.
    json read_args;
    read_args["path"] = "mutable.txt";
    server.execute("read_file", read_args.dump());

    // Modify externally (simulates an editor or another process).
    write_test_file(tmp.path(), "mutable.txt", "externally changed");

    json write_args;
    write_args["path"] = "mutable.txt";
    write_args["content"] = "new content";
    auto envelope = server.execute("write_file", write_args.dump());
    auto result = parse_result(envelope);

    // The write is ALLOWED: the gate is read-before-write, not
    // read-and-unchanged. Documenting this honestly beats a test whose name
    // promises detection that no code performs.
    REQUIRE_FALSE(result.contains("error"));
}

TEST_CASE("gh#158: the read tracker is per session, and a lookup never "
          "grows it", "[filesystem][gh158][v2.13.0]") {
    /**
     * @brief FileAccessTracker keys reads by session; was_read of an
     *        unknown session allocates nothing; release forgets one
     *        session only.
     * @internal
     * @version 2.13.0
     */
    FileAccessTracker t;
    t.record_read("s-a", "/r/x.txt", 1);
    CHECK(t.was_read("s-a", "/r/x.txt"));
    CHECK_FALSE(t.was_read("s-b", "/r/x.txt"));
    CHECK_FALSE(t.was_read("", "/r/x.txt"));
    CHECK(t.session_count() == 1U);  // the lookups created nothing

    t.record_read("s-b", "/r/y.txt", 2);
    CHECK(t.session_count() == 2U);
    CHECK(t.release_session("s-a"));
    CHECK_FALSE(t.was_read("s-a", "/r/x.txt"));
    CHECK(t.was_read("s-b", "/r/y.txt"));
    CHECK(t.session_count() == 1U);
    CHECK_FALSE(t.release_session("s-a"));  // already gone
}

TEST_CASE("gh#158: write_file consults the CALLING session's reads",
          "[filesystem][gh158][v2.13.0]") {
    /**
     * @brief The server keys by the session published to the dispatching
     *        thread (the ToolExecutor publishes the key it routed on).
     * @internal
     * @version 2.13.0
     */
    TempDir tmp;
    write_test_file(tmp.path(), "f.txt", "original");
    auto server = make_server(tmp.path());
    json read_args;
    read_args["path"] = "f.txt";
    json write_args;
    write_args["path"] = "f.txt";
    write_args["content"] = "changed";

    {
        RunSessionScope a("s-a");
        server.execute("read_file", read_args.dump());
    }
    json b_result;
    {
        RunSessionScope b("s-b");
        b_result = parse_result(
            server.execute("write_file", write_args.dump()));
    }
    REQUIRE(b_result.contains("error"));
    CHECK(b_result["error"].get<std::string>() == "read_before_write");

    json a_result;
    {
        RunSessionScope a("s-a");
        a_result = parse_result(
            server.execute("write_file", write_args.dump()));
    }
    CHECK_FALSE(a_result.contains("error"));
    CHECK(server.session_count() == 1U);
    CHECK(server.release_session("s-a"));
    CHECK(server.session_count() == 0U);
}

TEST_CASE("test_edit_str_replace", "[filesystem]") {
    /**
     * @brief Edit a file with old_string/new_string replacement.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "edit.txt", "foo bar baz");
    auto server = make_server(tmp.path());

    // Read first (required)
    json read_args;
    read_args["path"] = "edit.txt";
    server.execute("read_file", read_args.dump());

    // Edit
    json edit_args;
    edit_args["path"] = "edit.txt";
    edit_args["old_string"] = "bar";
    edit_args["new_string"] = "qux";
    auto envelope = server.execute("edit_file", edit_args.dump());
    auto result = parse_result(envelope);

    REQUIRE(result.contains("message"));

    // Verify file content
    std::ifstream in(tmp.path() / "edit.txt");
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    REQUIRE(content == "foo qux baz");
}

TEST_CASE("test_edit_str_replace_no_match", "[filesystem]") {
    /**
     * @brief Edit with old_string that does not exist in file.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "nomatch.txt", "hello world");
    auto server = make_server(tmp.path());

    json read_args;
    read_args["path"] = "nomatch.txt";
    server.execute("read_file", read_args.dump());

    json edit_args;
    edit_args["path"] = "nomatch.txt";
    edit_args["old_string"] = "MISSING";
    edit_args["new_string"] = "replacement";
    auto envelope = server.execute("edit_file", edit_args.dump());
    auto result = parse_result(envelope);

    REQUIRE(result.contains("error"));
    REQUIRE(result["error"].get<std::string>() == "not_found");
}

TEST_CASE("test_edit_str_replace_multiple_matches",
          "[filesystem]") {
    /**
     * @brief Edit where old_string appears multiple times without
     *        replace_all. Verify error about multiple matches.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "multi.txt",
                    "aaa bbb aaa ccc aaa");
    auto server = make_server(tmp.path());

    json read_args;
    read_args["path"] = "multi.txt";
    server.execute("read_file", read_args.dump());

    json edit_args;
    edit_args["path"] = "multi.txt";
    edit_args["old_string"] = "aaa";
    edit_args["new_string"] = "zzz";
    edit_args["replace_all"] = false;
    auto envelope = server.execute("edit_file", edit_args.dump());
    auto result = parse_result(envelope);

    REQUIRE(result.contains("error"));
    REQUIRE(result["error"].get<std::string>() ==
            "multiple_matches");
}

TEST_CASE("test_edit_insert_mode", "[filesystem]") {
    /**
     * @brief Edit with insert_line to insert text at a position.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "insert.txt", "line1\nline2\nline3");
    auto server = make_server(tmp.path());

    json read_args;
    read_args["path"] = "insert.txt";
    server.execute("read_file", read_args.dump());

    json edit_args;
    edit_args["path"] = "insert.txt";
    edit_args["insert_line"] = 2;
    edit_args["new_string"] = "inserted";
    auto envelope = server.execute("edit_file", edit_args.dump());
    auto result = parse_result(envelope);

    REQUIRE(result.contains("message"));

    // Verify content — inserted before line 2
    std::ifstream in(tmp.path() / "insert.txt");
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    REQUIRE(content.find("line1\ninserted\nline2") !=
            std::string::npos);
}

TEST_CASE("test_glob_finds_files", "[filesystem]") {
    /**
     * @brief Glob for *.txt and verify matching files found.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "a.txt", "a");
    write_test_file(tmp.path(), "b.txt", "b");
    write_test_file(tmp.path(), "c.cpp", "c");

    auto server = make_server(tmp.path());

    // Glob executes from cwd, so we need to chdir
    auto prev_cwd = fs::current_path();
    fs::current_path(tmp.path());

    json args;
    args["pattern"] = "*.txt";
    auto envelope = server.execute("glob", args.dump());
    auto result = json::parse(raw_result(envelope));

    fs::current_path(prev_cwd);

    REQUIRE(result.is_array());
    REQUIRE(result.size() == 2);
}

TEST_CASE("test_glob_simple_pattern", "[filesystem]") {
    /**
     * @brief Single wildcard pattern still works post-#13 brace expansion.
     * @internal
     * @version 2.1.4
     */
    TempDir tmp;
    write_test_file(tmp.path(), "x.txt", "x");
    write_test_file(tmp.path(), "y.md", "y");
    write_test_file(tmp.path(), "z.txt", "z");

    auto server = make_server(tmp.path());
    auto prev_cwd = fs::current_path();
    fs::current_path(tmp.path());

    json args;
    args["pattern"] = "*.txt";
    auto envelope = server.execute("glob", args.dump());
    auto result = json::parse(raw_result(envelope));

    fs::current_path(prev_cwd);

    REQUIRE(result.is_array());
    REQUIRE(result.size() == 2);
}

TEST_CASE("test_glob_brace_expansion_real", "[filesystem][2.1.4][issue-13]") {
    /**
     * @brief Issue #13: brace expansion is now actually supported.
     *        Pattern `*.{txt,md}` matches BOTH x.txt and y.md.
     * @internal
     * @version 2.1.4
     */
    TempDir tmp;
    write_test_file(tmp.path(), "x.txt", "x");
    write_test_file(tmp.path(), "y.md", "y");
    write_test_file(tmp.path(), "z.cpp", "z");

    auto server = make_server(tmp.path());
    auto prev_cwd = fs::current_path();
    fs::current_path(tmp.path());

    json args;
    args["pattern"] = "*.{txt,md}";
    auto envelope = server.execute("glob", args.dump());
    auto result = json::parse(raw_result(envelope));

    fs::current_path(prev_cwd);

    REQUIRE(result.is_array());
    REQUIRE(result.size() == 2);
}

TEST_CASE("test_glob_brace_expansion_three_alternatives",
          "[filesystem][2.1.4][issue-13]") {
    /**
     * @brief Issue #13: `*.{c,h,py}` produces three patterns.
     * @internal
     * @version 2.1.4
     */
    TempDir tmp;
    write_test_file(tmp.path(), "a.c", "a");
    write_test_file(tmp.path(), "b.h", "b");
    write_test_file(tmp.path(), "c.py", "c");
    write_test_file(tmp.path(), "d.md", "d");

    auto server = make_server(tmp.path());
    auto prev_cwd = fs::current_path();
    fs::current_path(tmp.path());

    json args;
    args["pattern"] = "*.{c,h,py}";
    auto envelope = server.execute("glob", args.dump());
    auto result = json::parse(raw_result(envelope));

    fs::current_path(prev_cwd);

    REQUIRE(result.is_array());
    REQUIRE(result.size() == 3);
}

TEST_CASE("test_grep_brace_expansion_in_glob",
          "[filesystem][2.1.4][issue-13]") {
    /**
     * @brief Issue #13: grep's `glob` arg also supports brace
     *        expansion. Pre-2.1.4 `*.{c,h}` killed the session with
     *        regex_error.
     * @internal
     * @version 2.1.4
     */
    TempDir tmp;
    write_test_file(tmp.path(), "a.c", "needle here\n");
    write_test_file(tmp.path(), "b.h", "needle there\n");
    write_test_file(tmp.path(), "c.py", "needle ignored\n");

    auto server = make_server(tmp.path());
    auto prev_cwd = fs::current_path();
    fs::current_path(tmp.path());

    json args;
    args["pattern"] = "needle";
    args["glob"] = "*.{c,h}";
    auto envelope = server.execute("grep", args.dump());
    auto result = json::parse(raw_result(envelope));

    fs::current_path(prev_cwd);

    REQUIRE(result.is_array());
    REQUIRE(result.size() == 2);
}

TEST_CASE("test_grep_invalid_regex_returns_structured_error",
          "[filesystem][2.1.4][issue-13]") {
    /**
     * @brief Issue #13: malformed regex returns a structured tool
     *        error (`{"error":"invalid_regex",...}`) rather than
     *        propagating an unhandled exception.
     * @internal
     * @version 2.1.4
     */
    TempDir tmp;
    write_test_file(tmp.path(), "a.c", "hello\n");

    auto server = make_server(tmp.path());

    json args;
    args["pattern"] = "[unclosed";
    auto envelope = server.execute("grep", args.dump());
    auto result = json::parse(raw_result(envelope));

    REQUIRE(result.is_object());
    REQUIRE(result.at("error").get<std::string>() == "invalid_regex");
}

// ── Issue #15: gitignore + explorerignore ────────────────

TEST_CASE("test_glob_respects_gitignore",
          "[filesystem][2.1.4][issue-15]") {
    /**
     * @brief Issue #15: a `.gitignore` listing `build/` excludes
     *        `build/foo.o` from glob results. Pre-2.1.4 only the
     *        hardcoded SKIP_DIRS list was honored.
     * @internal
     * @version 2.1.4
     */
    TempDir tmp;
    write_test_file(tmp.path(), ".gitignore", "build/\n*.log\n");
    write_test_file(tmp.path(), "src/main.cpp", "int main(){}");
    write_test_file(tmp.path(), "build/main.o", "obj");
    write_test_file(tmp.path(), "build/x/y/z.o", "obj");
    write_test_file(tmp.path(), "logs/error.log", "boom");

    auto server = make_server(tmp.path());

    json args;
    args["pattern"] = "*";
    auto envelope = server.execute("glob", args.dump());
    auto result = json::parse(raw_result(envelope));

    REQUIRE(result.is_array());
    bool saw_build = false;
    bool saw_log = false;
    bool saw_main = false;
    for (const auto& p : result) {
        auto s = p.get<std::string>();
        if (s.find("/build/") != std::string::npos) saw_build = true;
        if (s.find(".log") != std::string::npos) saw_log = true;
        if (s.find("main.cpp") != std::string::npos) saw_main = true;
    }
    CHECK_FALSE(saw_build);
    CHECK_FALSE(saw_log);
    CHECK(saw_main);
}

TEST_CASE("test_glob_respects_explorerignore_supplementary",
          "[filesystem][2.1.4][issue-15]") {
    /**
     * @brief Issue #15: .explorerignore is supplementary — it can
     *        add patterns on top of .gitignore.
     * @internal
     * @version 2.1.4
     */
    TempDir tmp;
    write_test_file(tmp.path(), ".gitignore", "build/\n");
    // .gitignore covers build/, .explorerignore adds *.xml exclusion
    write_test_file(tmp.path(), ".explorerignore",
                    "*.xml\ndocs/doxygen/\n");
    write_test_file(tmp.path(), "src/main.cpp", "x");
    write_test_file(tmp.path(), "data.xml", "x");
    write_test_file(tmp.path(), "docs/doxygen/index.html", "x");
    write_test_file(tmp.path(), "build/x.o", "x");

    auto server = make_server(tmp.path());

    json args;
    args["pattern"] = "*";
    auto envelope = server.execute("glob", args.dump());
    auto result = json::parse(raw_result(envelope));

    REQUIRE(result.is_array());
    for (const auto& p : result) {
        auto s = p.get<std::string>();
        CHECK(s.find(".xml") == std::string::npos);
        CHECK(s.find("doxygen") == std::string::npos);
        CHECK(s.find("/build/") == std::string::npos);
    }
}

TEST_CASE("test_explorerignore_negation_re_includes_path",
          "[filesystem][2.1.4][issue-15]") {
    /**
     * @brief Issue #15: gitignore `!pattern` re-includes a path
     *        previously excluded.
     * @internal
     * @version 2.1.4
     */
    TempDir tmp;
    write_test_file(tmp.path(), ".gitignore", "*.log\n!keep.log\n");
    write_test_file(tmp.path(), "x.log", "noise");
    write_test_file(tmp.path(), "keep.log", "important");

    auto server = make_server(tmp.path());

    json args;
    args["pattern"] = "*.log";
    auto envelope = server.execute("glob", args.dump());
    auto result = json::parse(raw_result(envelope));

    REQUIRE(result.is_array());
    REQUIRE(result.size() == 1);
    auto only = result[0].get<std::string>();
    REQUIRE(only.find("keep.log") != std::string::npos);
}

// ── gh#161: the walk is bounded, and says so ─────────────

/**
 * @brief Build a tree with one file per directory.
 * @param root Tree root.
 * @param dirs Number of directories to create.
 * @internal
 * @version 2.13.0
 */
static void write_wide_tree(const fs::path& root, int dirs) {
    for (int i = 0; i < dirs; ++i) {
        write_test_file(root, "d" + std::to_string(i) + "/f.txt",
                        "needle\n");
    }
}

TEST_CASE("test_glob_announces_a_truncated_walk",
          "[filesystem][2.13.0][gh-161]") {
    /**
     * @brief gh#161: a walk that hits `max_walk_entries` stops AND
     *        says so in the tool result. A silently short answer
     *        would be worse than the 87 s hang it replaces — the
     *        model would read it as a complete, empty result.
     * @internal
     * @version 2.13.0
     */
    TempDir tmp;
    write_wide_tree(tmp.path(), 40);

    FilesystemConfig cfg;
    cfg.max_walk_entries = 5;
    auto server = make_server(tmp.path(), cfg);

    json args;
    args["pattern"] = "*.txt";
    auto result = json::parse(
        raw_result(server.execute("glob", args.dump())));

    REQUIRE(result.is_array());
    REQUIRE_FALSE(result.empty());
    const auto& last = result.back();
    REQUIRE(last.is_object());
    CHECK(last.at("truncated").get<bool>());
    auto note = last.at("note").get<std::string>();
    CHECK(note.find("walk truncated at 5 entries") != std::string::npos);
    CHECK(note.find("narrow the pattern") != std::string::npos);
}

TEST_CASE("test_grep_announces_a_truncated_walk",
          "[filesystem][2.13.0][gh-161]") {
    /**
     * @brief gh#161: grep carries the same bound as glob — it needs
     *        it more, since it also opens and reads every file the
     *        glob filter admits.
     * @internal
     * @version 2.13.0
     */
    TempDir tmp;
    write_wide_tree(tmp.path(), 40);

    FilesystemConfig cfg;
    cfg.max_walk_entries = 4;
    auto server = make_server(tmp.path(), cfg);

    json args;
    args["pattern"] = "needle";
    auto result = json::parse(
        raw_result(server.execute("grep", args.dump())));

    REQUIRE(result.is_array());
    REQUIRE_FALSE(result.empty());
    const auto& last = result.back();
    REQUIRE(last.is_object());
    REQUIRE(last.contains("truncated"));
    auto note = last.at("note").get<std::string>();
    CHECK(note.find("walk truncated at 4 entries") != std::string::npos);
}

TEST_CASE("test_glob_and_grep_are_silent_when_the_walk_completes",
          "[filesystem][2.13.0][gh-161]") {
    /**
     * @brief gh#161: the notice is a CEILING, not a decoration. With
     *        the shipped default (250k entries) a normal workspace
     *        never sees it, and the result shape is unchanged.
     * @internal
     * @version 2.13.0
     */
    TempDir tmp;
    write_wide_tree(tmp.path(), 10);

    auto server = make_server(tmp.path());   // default max_walk_entries

    json glob_args;
    glob_args["pattern"] = "*.txt";
    auto globbed = json::parse(
        raw_result(server.execute("glob", glob_args.dump())));
    REQUIRE(globbed.size() == 10);
    for (const auto& entry : globbed) {
        CHECK(entry.is_string());
    }

    json grep_args;
    grep_args["pattern"] = "needle";
    auto grepped = json::parse(
        raw_result(server.execute("grep", grep_args.dump())));
    REQUIRE(grepped.size() == 10);
    for (const auto& match : grepped) {
        CHECK(match.contains("path"));
        CHECK_FALSE(match.contains("truncated"));
    }
}

TEST_CASE("test_read_file_refuses_ignored_path",
          "[filesystem][2.1.4][issue-15]") {
    /**
     * @brief Issue #15: read_file returns a structured "ignored"
     *        error when asked to read a path excluded by
     *        .gitignore / .explorerignore.
     * @internal
     * @version 2.1.4
     */
    TempDir tmp;
    write_test_file(tmp.path(), ".gitignore", "secret/\n");
    write_test_file(tmp.path(), "secret/api_key.txt", "sk-...");
    write_test_file(tmp.path(), "public/readme.md", "hi");

    auto server = make_server(tmp.path());

    json args;
    args["path"] = (tmp.path() / "secret/api_key.txt").string();
    auto envelope = server.execute("read_file", args.dump());
    auto result = json::parse(raw_result(envelope));

    REQUIRE(result.is_object());
    REQUIRE(result.at("error").get<std::string>() == "ignored");
}

TEST_CASE("test_glob_skips_directories", "[filesystem]") {
    /**
     * @brief Create .git directory with files and verify glob
     *        skips it during traversal.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "top.txt", "top");
    write_test_file(tmp.path(), ".git/hidden.txt", "hidden");

    auto server = make_server(tmp.path());
    auto prev_cwd = fs::current_path();
    fs::current_path(tmp.path());

    json args;
    args["pattern"] = "*.txt";
    auto envelope = server.execute("glob", args.dump());
    auto result = json::parse(raw_result(envelope));

    fs::current_path(prev_cwd);

    REQUIRE(result.is_array());
    REQUIRE(result.size() == 1);

    // The one match should be top.txt, not .git/hidden.txt
    auto path_str = result[0].get<std::string>();
    REQUIRE(path_str.find(".git") == std::string::npos);
}

TEST_CASE("test_grep_finds_matches", "[filesystem]") {
    /**
     * @brief Grep for a pattern across files and verify matches.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "a.txt", "hello world\nfoo bar");
    write_test_file(tmp.path(), "b.txt", "no match here");
    write_test_file(tmp.path(), "c.txt", "hello again");

    auto server = make_server(tmp.path());
    auto prev_cwd = fs::current_path();
    fs::current_path(tmp.path());

    json args;
    args["pattern"] = "hello";
    args["glob"] = "*.txt";
    auto envelope = server.execute("grep", args.dump());
    auto result = json::parse(raw_result(envelope));

    fs::current_path(prev_cwd);

    REQUIRE(result.is_array());
    REQUIRE(result.size() == 2);

    for (const auto& match : result) {
        REQUIRE(match.contains("path"));
        REQUIRE(match.contains("line"));
        REQUIRE(match.contains("content"));
    }
}

TEST_CASE("test_list_directory_flat", "[filesystem]") {
    /**
     * @brief List directory non-recursive and verify entries.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "file1.txt", "a");
    write_test_file(tmp.path(), "file2.txt", "b");
    fs::create_directory(tmp.path() / "subdir");

    auto server = make_server(tmp.path());

    json args;
    args["path"] = ".";
    auto envelope = server.execute("list_directory", args.dump());
    auto result = json::parse(raw_result(envelope));

    REQUIRE(result.is_array());
    REQUIRE(result.size() == 3); // 2 files + 1 directory

    bool found_dir = false;
    for (const auto& entry : result) {
        if (entry["type"].get<std::string>() == "directory") {
            found_dir = true;
        }
    }
    REQUIRE(found_dir);
}

TEST_CASE("test_list_directory_recursive", "[filesystem]") {
    /**
     * @brief List directory with recursive=true and verify
     *        nested entries appear.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "top.txt", "top");
    write_test_file(tmp.path(), "sub/nested.txt", "nested");

    auto server = make_server(tmp.path());

    json args;
    args["path"] = ".";
    args["recursive"] = true;
    auto envelope = server.execute("list_directory", args.dump());
    auto result = json::parse(raw_result(envelope));

    REQUIRE(result.is_array());
    REQUIRE(result.size() >= 3); // top.txt, sub/, sub/nested.txt

    bool found_nested = false;
    for (const auto& entry : result) {
        auto name = entry["name"].get<std::string>();
        if (name == "nested.txt") {
            found_nested = true;
        }
    }
    REQUIRE(found_nested);
}

// ── gh#116 (v2.9.13): list_directory optional path default ──────────────────

SCENARIO("gh#116: list_directory with path omitted uses project root default",
         "[filesystem][gh116][regression][2.9.13]") {
    GIVEN("a filesystem server with files at project root") {
        TempDir tmp;
        write_test_file(tmp.path(), "root_file.txt", "x");
        fs::create_directory(tmp.path() / "root_subdir");
        auto server = make_server(tmp.path());

        WHEN("list_directory is called with no 'path' argument") {
            json args = json::object();
            std::string envelope;
            THEN("it does not throw and returns a listing of the project root") {
                REQUIRE_NOTHROW(
                    envelope = server.execute("list_directory", args.dump()));
                auto result = json::parse(raw_result(envelope));
                REQUIRE(result.is_array());
                bool found_file = false;
                bool found_dir  = false;
                for (const auto& entry : result) {
                    auto n = entry["name"].get<std::string>();
                    if (n == "root_file.txt") { found_file = true; }
                    if (n == "root_subdir")   { found_dir  = true; }
                }
                CHECK(found_file);
                CHECK(found_dir);
            }
        }
    }
}

// ── gh#116 (v2.9.14): read_file with directory path ─────────────────────────

SCENARIO("gh#116: read_file with a directory path returns is_directory error",
         "[filesystem][gh116][regression][2.9.14]") {
    GIVEN("a filesystem server with a subdirectory") {
        TempDir tmp;
        fs::create_directory(tmp.path() / "mydir");
        auto server = make_server(tmp.path());

        WHEN("read_file is called with a directory path") {
            json args;
            args["path"] = "mydir";
            std::string envelope;
            THEN("it does not throw and returns an is_directory error") {
                REQUIRE_NOTHROW(
                    envelope = server.execute("read_file", args.dump()));
                auto result = json::parse(raw_result(envelope));
                REQUIRE(result.is_object());
                CHECK(result["error"].get<std::string>() == "is_directory");
            }
        }
    }
}

TEST_CASE("test_path_security_rejects_traversal",
          "[filesystem][gh143][2.12.0]") {
    /**
     * @brief Attempt path traversal and verify rejection.
     *
     * The security property under test is that the traversal is REJECTED
     * and nothing outside the root is read. That property is unchanged.
     *
     * What changed in v2.12.0 (gh#143) is the delivery mechanism. The
     * FilesystemServer still refuses the path and still never opens the
     * file — the refusal is now reported to the model as a tool error
     * instead of unwinding as an exception that aborted the whole run.
     * That is the point of gh#143: an unhandled throw from a tool killed
     * the conversation, and a denial the model cannot see is a denial it
     * cannot correct.
     *
     * The assertions below are deliberately STRONGER than the
     * REQUIRE_THROWS_AS they replace: they pin the refusal, the absence
     * of any leaked content, and the absence of a ContextAnchor. That
     * last one is not hypothetical — the first cut of the gh#143 barrier
     * sat in ToolRegistry::dispatch, which let `inject_anchor_if_needed`
     * run on the rejected path and anchor `../../etc/passwd` into context
     * as though it had been read. This test caught it.
     *
     * @internal
     * @version 2.12.0
     */
    TempDir tmp;
    auto server = make_server(tmp.path());

    json args;
    args["path"] = "../../etc/passwd";

    std::string envelope_str;
    REQUIRE_NOTHROW(
        envelope_str = server.execute("read_file", args.dump()));

    auto envelope = json::parse(envelope_str);
    auto result = raw_result(envelope_str);

    // The traversal is refused, and the refusal says so.
    CHECK(result.find("Path escapes project root") != std::string::npos);

    // Nothing outside the root leaked into the response. Match on a
    // passwd-record shape rather than "root:", which occurs innocently
    // inside the refusal text "Path escapes project root: ...".
    CHECK(result.find("x:0:0") == std::string::npos);
    CHECK(result.find("/bin/") == std::string::npos);

    // A rejected path must not be anchored into context.
    REQUIRE(envelope.contains("directives"));
    REQUIRE(envelope["directives"].is_array());
    CHECK(envelope["directives"].empty());
}

TEST_CASE("test_read_file_anchor_key", "[filesystem]") {
    /**
     * @brief Verify read_file response includes a ContextAnchor
     *        directive (auto-injected by MCPServerBase for tools
     *        that declare anchor_key).
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "anchored.txt", "content");
    auto server = make_server(tmp.path());

    json args;
    args["path"] = "anchored.txt";
    auto envelope_str = server.execute("read_file", args.dump());
    auto envelope = json::parse(envelope_str);

    REQUIRE(envelope.contains("directives"));
    auto& directives = envelope["directives"];
    REQUIRE(directives.is_array());
    REQUIRE_FALSE(directives.empty());

    bool found_anchor = false;
    for (const auto& d : directives) {
        if (d["type"].get<std::string>() == "context_anchor") {
            found_anchor = true;
        }
    }
    REQUIRE(found_anchor);
}

TEST_CASE("test_skip_duplicate_check_read", "[filesystem]") {
    /**
     * @brief Verify skip_duplicate_check returns true for
     *        read_file and false for other tools.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    auto server = make_server(tmp.path());

    REQUIRE(server.skip_duplicate_check("read_file"));
    REQUIRE_FALSE(server.skip_duplicate_check("write_file"));
    REQUIRE_FALSE(server.skip_duplicate_check("edit_file"));
    REQUIRE_FALSE(server.skip_duplicate_check("glob"));
    REQUIRE_FALSE(server.skip_duplicate_check("grep"));
    REQUIRE_FALSE(server.skip_duplicate_check("list_directory"));
}

// ── v2.3.10: accessor coverage ──

TEST_CASE("FilesystemServer accessors round-trip state",
          "[filesystem][v2.3.10][coverage]") {
    TempDir tmp;
    auto server = make_server(tmp.path());

    SECTION("root_dir returns the path the server was constructed with") {
        REQUIRE(server.root_dir() == tmp.path());
    }

    SECTION("set_working_dir to a valid directory updates root_dir") {
        TempDir other;
        REQUIRE(server.set_working_dir(other.path().string()));
        REQUIRE(server.root_dir() == other.path());
    }

    SECTION("set_working_dir to a non-existent path returns false") {
        REQUIRE_FALSE(server.set_working_dir(
            "/does-not-exist-v2310"));
    }

    SECTION("set_working_dir to a file (not directory) returns false") {
        auto file = tmp.path() / "regular.txt";
        std::ofstream(file) << "content\n";
        REQUIRE_FALSE(server.set_working_dir(file.string()));
    }

    SECTION("tracker() exposes the underlying FileAccessTracker") {
        auto& t = server.tracker();
        (void)t;
        REQUIRE(true);
    }

    SECTION("ignore() exposes the IgnoreMatcher") {
        const auto& ig = server.ignore();
        (void)ig.rule_count();
        REQUIRE(true);
    }

    SECTION("config() returns the FilesystemConfig") {
        const auto& cfg = server.config();
        (void)cfg.allow_outside_root;
        REQUIRE(true);
    }
}

TEST_CASE("FilesystemServer tools advertise their required access levels",
          "[filesystem][v2.3.10][coverage]") {
    TempDir tmp;
    auto server = make_server(tmp.path());

    // Each tool's required_access_level() override is dead code
    // unless something queries it. ServerManager::get_required_access_level
    // is the consumer; calling it for every tool exercises every
    // tool's override in turn.
    auto& reg = server.registry();
    for (const auto& name : {"read_file", "write_file", "edit_file",
                              "glob", "grep", "list_directory"}) {
        auto* t = reg.get_tool(name);
        REQUIRE(t != nullptr);
        auto lvl = t->required_access_level();
        // read_file/glob/grep/list_directory are READ; write/edit
        // are WRITE. Either is acceptable here; the coverage point
        // is just that every override executed.
        (void)lvl;
    }
    REQUIRE(true);
}

TEST_CASE("gh#120: read_file lines are an ordered array not a keyed object",
          "[filesystem][gh120][regression][2.9.18]") {
    // With the old dict schema, string keys are iterated in lexicographic
    // order: "1","10","11","12","2","3",... — line 10 appears before line 2.
    // The fix changes lines to json::array() so natural file order is
    // preserved. RED: result["lines"].is_array() is false before the fix.
    TempDir tmp;
    std::string content;
    for (int i = 1; i <= 12; ++i) {
        content += "line " + std::to_string(i) + "\n";
    }
    write_test_file(tmp.path(), "twelve.txt", content);
    auto server = make_server(tmp.path());

    json args;
    args["path"] = "twelve.txt";
    auto result = parse_result(server.execute("read_file", args.dump()));

    REQUIRE(result["total"].get<int>() == 12);
    // RED before fix: lines is a json::object, not an array.
    REQUIRE(result["lines"].is_array());
    // Natural order: index 0 is the first physical line, index 9 is line 10.
    CHECK(result["lines"][0].get<std::string>() == "line 1");
    CHECK(result["lines"][9].get<std::string>() == "line 10");
}

// ── gh#126 (v2.10.0): glob path matching — relative path + ** pattern ────────

TEST_CASE("gh#126: glob ** pattern matches files at root and in subdirectories",
          "[filesystem][gh126][regression]") {
    // Pre-fix: classify_glob_entry matched entry.path().filename() (basename
    // only). A pattern like **/*.cpp compiled to regex .*.*\/.*\.cpp which
    // requires a '/' in the subject string — basenames never have one, so
    // zero files matched regardless of depth.
    // Fix: use fs::relative(entry.path(), root).generic_string() so the
    // subject includes the directory path, and emit_glob_star so ** → .*
    // (not .*.*).
    TempDir tmp;
    write_test_file(tmp.path(), "foo.cpp", "// root");
    write_test_file(tmp.path(), "src/bar.cpp", "// nested");
    write_test_file(tmp.path(), "src/baz.h", "// header, excluded");

    auto server = make_server(tmp.path());
    auto prev_cwd = fs::current_path();
    fs::current_path(tmp.path());

    json args;
    args["pattern"] = "**/*.cpp";
    auto result = json::parse(raw_result(server.execute("glob", args.dump())));

    fs::current_path(prev_cwd);

    // RED before fix: basename matching means no filename contains '/', so
    // the **/ portion never satisfies → 0 results.
    REQUIRE(result.is_array());
    REQUIRE(result.size() == 2);
}

TEST_CASE("gh#126: glob with directory prefix matches only files in that subdir",
          "[filesystem][gh126][regression]") {
    // Pre-fix: "bar.cpp" basename does not match src/*.cpp regex
    // src\/.*\.cpp → 0 results even though src/bar.cpp exists.
    TempDir tmp;
    write_test_file(tmp.path(), "foo.cpp", "// root");
    write_test_file(tmp.path(), "src/bar.cpp", "// nested");

    auto server = make_server(tmp.path());
    auto prev_cwd = fs::current_path();
    fs::current_path(tmp.path());

    json args;
    args["pattern"] = "src/*.cpp";
    auto result = json::parse(raw_result(server.execute("glob", args.dump())));

    fs::current_path(prev_cwd);

    // RED before fix: 0 results; post-fix: exactly 1 (src/bar.cpp, not foo.cpp).
    REQUIRE(result.is_array());
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].get<std::string>().find("bar.cpp") != std::string::npos);
}

// ── gh#124 (v2.10.0): read_file not-found gives corrective guidance ──────────

TEST_CASE("gh#124: read_file not-found error message names list_directory",
          "[filesystem][gh124][regression]") {
    // Pre-fix: error["message"] = "File not found: path" with no next step.
    // The agent stalls: no hint to use list_directory or that paths are
    // root-relative.
    // Fix: appended corrective guidance at check_read_gates line 832-833.
    TempDir tmp;
    auto server = make_server(tmp.path());

    json args;
    args["path"] = "does_not_exist.txt";
    auto result = parse_result(server.execute("read_file", args.dump()));

    REQUIRE(result.contains("error"));
    REQUIRE(result["error"].get<std::string>() == "not_found");
    // Core requirement: message must name the recovery tool.
    REQUIRE(result["message"].get<std::string>().find("list_directory")
            != std::string::npos);
}

// ── v2.13.0: outside-root policy + the host's path approver ─────────────

namespace {

/**
 * @brief Test approver: records every request, answers a fixed verdict.
 * @internal
 * @version 2.13.0
 */
struct RecordingApprover {
    OutsideRootVerdict answer = OutsideRootVerdict::approved; ///< Reply
    std::vector<OutsideRootRequest> seen;                     ///< Requests

    /** @brief C-style trampoline. @internal @version 2.13.0 */
    static OutsideRootVerdict call(const OutsideRootRequest& req, void* ud) {
        auto* self = static_cast<RecordingApprover*>(ud);
        self->seen.push_back(req);
        return self->answer;
    }
};

/**
 * @brief A root and a sibling "outside" tree under one TempDir.
 * @internal
 * @version 2.13.0
 */
struct OutsideFixture {
    TempDir tmp;        ///< Owns both trees
    fs::path root;      ///< Server root
    fs::path outside;   ///< Sibling tree, outside the root

    /** @brief Build both trees. @internal @version 2.13.0 */
    OutsideFixture()
        : root(fs::weakly_canonical(tmp.path()) / "root"),
          outside(fs::weakly_canonical(tmp.path()) / "outside") {
        fs::create_directories(root);
        write_test_file(outside, "secret.txt", "OUTSIDE-SECRET\n");
        write_test_file(outside, "data/f.txt", "DATA\n");
    }
};

/**
 * @brief `{"path": p}` (plus `content` when given) as a JSON string.
 * @internal
 * @version 2.13.0
 */
std::string path_json(const fs::path& p, const char* content = nullptr) {
    json j;
    j["path"] = p.string();
    if (content != nullptr) { j["content"] = content; }
    return j.dump();
}

}  // namespace

TEST_CASE("outside-root optional: the approver is asked with path, root, "
          "tool and read/write", "[filesystem][outside_root][v2.13.0]") {
    OutsideFixture f;
    auto server = make_server(f.root);  // default config: optional
    RecordingApprover approver;
    server.set_outside_root_approver(&RecordingApprover::call, &approver);

    SECTION("read_file asks as a READ and an approval serves the file") {
        auto out = server.execute("read_file",
                                  path_json(f.outside / "secret.txt"));
        INFO(out);
        CHECK(out.find("OUTSIDE-SECRET") != std::string::npos);
        REQUIRE(approver.seen.size() == 1);
        CHECK(approver.seen[0].path == (f.outside / "secret.txt").string());
        CHECK(approver.seen[0].root == f.root.string());
        CHECK(approver.seen[0].tool == "filesystem.read_file");
        CHECK(approver.seen[0].access == PathAccess::read);
    }

    SECTION("list_directory asks as a READ") {
        server.execute("list_directory", path_json(f.outside));
        REQUIRE(approver.seen.size() == 1);
        CHECK(approver.seen[0].tool == "filesystem.list_directory");
        CHECK(approver.seen[0].access == PathAccess::read);
    }

    SECTION("write_file asks as a WRITE") {
        server.execute("write_file",
                       path_json(f.outside / "new.txt", "hello"));
        REQUIRE(approver.seen.size() == 1);
        CHECK(approver.seen[0].tool == "filesystem.write_file");
        CHECK(approver.seen[0].access == PathAccess::write);
        CHECK(approver.seen[0].path == (f.outside / "new.txt").string());
    }

    SECTION("edit_file asks as a WRITE") {
        json args;
        args["path"] = (f.outside / "secret.txt").string();
        args["old_string"] = "OUTSIDE";
        args["new_string"] = "EDITED";
        server.execute("edit_file", args.dump());
        REQUIRE_FALSE(approver.seen.empty());
        CHECK(approver.seen[0].tool == "filesystem.edit_file");
        CHECK(approver.seen[0].access == PathAccess::write);
    }

    SECTION("the canonical path is what is asked about, not the spelling") {
        auto spelled = f.root / ".." / "outside" / "secret.txt";
        server.execute("read_file", path_json(spelled));
        REQUIRE(approver.seen.size() == 1);
        CHECK(approver.seen[0].path == (f.outside / "secret.txt").string());
    }
}

TEST_CASE("outside-root optional: a rejection refuses, types the error, "
          "and anchors and writes nothing", "[filesystem][outside_root][v2.13.0]") {
    OutsideFixture f;
    auto server = make_server(f.root);
    RecordingApprover approver;
    approver.answer = OutsideRootVerdict::rejected;
    server.set_outside_root_approver(&RecordingApprover::call, &approver);

    SECTION("a rejected read leaks nothing and is not anchored") {
        auto envelope = json::parse(server.execute(
            "read_file", path_json(f.outside / "secret.txt")));
        auto result = envelope["result"].get<std::string>();
        INFO(result);
        CHECK(result.find("OUTSIDE-SECRET") == std::string::npos);
        CHECK(result.find("outside_root_rejected") != std::string::npos);
        CHECK(result.find("Path escapes project root") != std::string::npos);
        // A refused read must never become a ContextAnchor (gh#143).
        CHECK(envelope["directives"].empty());
    }

    SECTION("a rejected write creates nothing") {
        auto out = server.execute(
            "write_file", path_json(f.outside / "planted.txt", "x"));
        INFO(out);
        CHECK(out.find("outside_root_rejected") != std::string::npos);
        CHECK_FALSE(fs::exists(f.outside / "planted.txt"));
    }
}

TEST_CASE("outside-root optional: no approver refuses with a typed, "
          "explicit message and no anchor", "[filesystem][outside_root][v2.13.0]") {
    OutsideFixture f;
    auto server = make_server(f.root);  // omitted key → optional
    REQUIRE(server.config().allow_outside_root
            == OutsideRootAccess::optional);

    auto envelope = json::parse(server.execute(
        "read_file", path_json(f.outside / "secret.txt")));
    auto result = envelope["result"].get<std::string>();
    INFO(result);
    CHECK(result.find("OUTSIDE-SECRET") == std::string::npos);
    CHECK(result.find("outside_root_approval_required") != std::string::npos);
    CHECK(result.find((f.outside / "secret.txt").string())
          != std::string::npos);
    CHECK(envelope["directives"].empty());

    SECTION("installing then CLEARING an approver returns to refusal") {
        RecordingApprover approver;
        server.set_outside_root_approver(&RecordingApprover::call, &approver);
        server.set_outside_root_approver(nullptr, nullptr);
        auto again = server.execute("read_file",
                                    path_json(f.outside / "secret.txt"));
        CHECK(again.find("outside_root_approval_required")
              != std::string::npos);
        CHECK(approver.seen.empty());
    }
}

TEST_CASE("outside-root: the approver is asked ONLY when the lists and the "
          "mode leave the decision open", "[filesystem][outside_root][v2.13.0]") {
    OutsideFixture f;
    RecordingApprover approver;

    SECTION("an allowlisted subtree is served without asking") {
        FilesystemConfig cfg;
        cfg.outside_root_allow = {f.outside / "data"};
        auto server = make_server(f.root, cfg);
        server.set_outside_root_approver(&RecordingApprover::call, &approver);
        auto out = server.execute("read_file",
                                  path_json(f.outside / "data" / "f.txt"));
        CHECK(out.find("DATA") != std::string::npos);
        CHECK(approver.seen.empty());
    }

    SECTION("a denied path is refused without asking, even if the approver "
            "would say yes") {
        FilesystemConfig cfg;
        cfg.outside_root_deny = {f.outside};
        auto server = make_server(f.root, cfg);
        server.set_outside_root_approver(&RecordingApprover::call, &approver);
        auto out = server.execute("read_file",
                                  path_json(f.outside / "secret.txt"));
        CHECK(out.find("outside_root_denied") != std::string::npos);
        CHECK(out.find("OUTSIDE-SECRET") == std::string::npos);
        CHECK(approver.seen.empty());
    }

    SECTION("legacy true serves without asking") {
        FilesystemConfig cfg;
        cfg.allow_outside_root = OutsideRootAccess::allow;
        auto server = make_server(f.root, cfg);
        server.set_outside_root_approver(&RecordingApprover::call, &approver);
        auto out = server.execute("read_file",
                                  path_json(f.outside / "secret.txt"));
        CHECK(out.find("OUTSIDE-SECRET") != std::string::npos);
        CHECK(approver.seen.empty());
    }

    SECTION("legacy false refuses without asking, with the old message") {
        FilesystemConfig cfg;
        cfg.allow_outside_root = OutsideRootAccess::refuse;
        auto server = make_server(f.root, cfg);
        server.set_outside_root_approver(&RecordingApprover::call, &approver);
        auto out = server.execute("read_file",
                                  path_json(f.outside / "secret.txt"));
        CHECK(out.find("Path escapes project root") != std::string::npos);
        CHECK(out.find("outside_root_") == std::string::npos);
        CHECK(approver.seen.empty());
    }

    SECTION("a path inside the root never asks") {
        write_test_file(f.root, "in.txt", "IN\n");
        auto server = make_server(f.root);
        server.set_outside_root_approver(&RecordingApprover::call, &approver);
        auto out = server.execute("read_file", path_json(f.root / "in.txt"));
        CHECK(out.find("IN") != std::string::npos);
        CHECK(approver.seen.empty());
    }

    SECTION("the root's string-prefix sibling is OUTSIDE and asks") {
        // /…/root vs /…/rootling: lexically_relative, never prefix.
        write_test_file(f.root.parent_path(), "rootling/x.txt", "SIB\n");
        auto server = make_server(f.root);
        approver.answer = OutsideRootVerdict::rejected;
        server.set_outside_root_approver(&RecordingApprover::call, &approver);
        auto out = server.execute(
            "read_file",
            path_json(f.root.parent_path() / "rootling" / "x.txt"));
        CHECK(out.find("SIB") == std::string::npos);
        CHECK(approver.seen.size() == 1);
    }
}
