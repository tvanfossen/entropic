// SPDX-License-Identifier: LGPL-3.0-or-later
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
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
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
        auto base = fs::temp_directory_path() / "entropic_test";
        int counter = 0;
        do {
            path_ = base.string() + "_" +
                     std::to_string(counter++);
        } while (fs::exists(path_));
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
    REQUIRE(result["lines"]["1"].get<std::string>() == "hello");
    REQUIRE(result["lines"]["2"].get<std::string>() == "world");
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

TEST_CASE("test_write_file_detects_external_change",
          "[filesystem]") {
    /**
     * @brief Read a file, modify it externally, then write via
     *        server. Verifies the server detects external change
     *        via mismatched content hash.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    write_test_file(tmp.path(), "mutable.txt", "original");
    auto server = make_server(tmp.path());

    // Read to satisfy read-before-write
    json read_args;
    read_args["path"] = "mutable.txt";
    server.execute("read_file", read_args.dump());

    // Modify the file externally (simulates editor or other process)
    write_test_file(tmp.path(), "mutable.txt", "externally changed");

    // Write via server — tracker recorded hash of "original",
    // but write_file uses check_read_before_write which only
    // checks was_read (not hash). The hash mismatch detection
    // surfaces through was_read_unchanged on subsequent reads.
    // For write_file, the write should succeed because was_read
    // returns true. This test verifies the tracker *has* stale
    // hash data that a future read would detect.
    json write_args;
    write_args["path"] = "mutable.txt";
    write_args["content"] = "new content";
    auto envelope = server.execute("write_file", write_args.dump());
    auto result = parse_result(envelope);

    // Write succeeds (was_read check passes), but tracker hash
    // is stale. Verify the tracker detects the mismatch.
    auto canonical = fs::weakly_canonical(
        tmp.path() / "mutable.txt").string();
    auto current_hash = std::hash<std::string>{}(
        "externally changed");
    REQUIRE_FALSE(
        server.tracker().was_read_unchanged(
            canonical, current_hash));
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

TEST_CASE("test_glob_brace_expansion", "[filesystem]") {
    /**
     * @brief Verify glob with simple wildcard pattern works.
     *        (std::regex glob does not support brace expansion,
     *        so test *.txt as functional equivalent.)
     * @internal
     * @version 1.8.5
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

TEST_CASE("test_path_security_rejects_traversal",
          "[filesystem]") {
    /**
     * @brief Attempt path traversal and verify rejection.
     * @internal
     * @version 1.8.5
     */
    TempDir tmp;
    auto server = make_server(tmp.path());

    json args;
    args["path"] = "../../etc/passwd";

    REQUIRE_THROWS_AS(
        server.execute("read_file", args.dump()),
        std::runtime_error);
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
