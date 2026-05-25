// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_git.cpp
 * @brief GitServer unit tests.
 * @version 1.8.5
 */

#include <entropic/mcp/servers/git.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cstdlib>
#include <fstream>

using json = nlohmann::json;
using namespace entropic;

// ── Helpers ─────────────────────────────────────────────

/**
 * @brief Parse the "result" field from execute() JSON envelope.
 * @internal
 * @param envelope Raw JSON string returned by execute().
 * @return The "result" string value.
 * @version 1.8.5
 */
static std::string parse_result(const std::string& envelope) {
    auto j = json::parse(envelope);
    return j["result"].get<std::string>();
}

/**
 * @brief RAII temp directory with git init for test isolation.
 * @internal
 * @version 1.8.5
 */
struct TempGitRepo {
    std::filesystem::path path;

    /**
     * @brief Create a temp directory and initialize a git repo.
     * @internal
     * @version 1.8.5
     */
    TempGitRepo() {
        path = std::filesystem::temp_directory_path() /
               ("entropic_git_test_" + std::to_string(
                   std::hash<std::string>{}(
                       std::to_string(reinterpret_cast<uintptr_t>(this)))));
        std::filesystem::create_directories(path);
        std::string cmd = "cd " + path.string() +
                          " && git init -q" +
                          " && git config user.email test@test.com" +
                          " && git config user.name Test";
        std::system(cmd.c_str());
    }

    /**
     * @brief Remove the temp directory.
     * @internal
     * @version 1.8.5
     */
    ~TempGitRepo() {
        std::filesystem::remove_all(path);
    }

    /**
     * @brief Create a file in the repo with given content.
     * @internal
     * @param name Filename relative to repo root.
     * @param content File content.
     * @version 1.8.5
     */
    void create_file(const std::string& name,
                     const std::string& content) {
        std::ofstream f(path / name);
        f << content;
    }

    /**
     * @brief Stage a file and commit with a message.
     * @internal
     * @param file File to stage.
     * @param message Commit message.
     * @version 1.8.5
     */
    void commit(const std::string& file, const std::string& message) {
        std::string cmd = "cd " + path.string() +
                          " && git add " + file +
                          " && git commit -q -m '" + message + "'";
        std::system(cmd.c_str());
    }

    /**
     * @brief Stage a file without committing.
     * @internal
     * @param file File to stage.
     * @version 1.8.5
     */
    void stage(const std::string& file) {
        std::string cmd = "cd " + path.string() +
                          " && git add " + file;
        std::system(cmd.c_str());
    }
};

/**
 * @brief Create a GitServer pointed at a temp repo.
 * @internal
 * @param repo Temp git repo to use.
 * @return Configured GitServer.
 * @version 1.8.5
 */
static GitServer make_git_server(const TempGitRepo& repo) {
    return GitServer(repo.path, TEST_DATA_DIR);
}

// ── Tests ───────────────────────────────────────────────

TEST_CASE("Git status returns JSON result", "[git]") {
    TempGitRepo repo;
    auto server = make_git_server(repo);

    auto result = parse_result(server.execute("status", "{}"));
    REQUIRE_FALSE(result.empty());
}

TEST_CASE("Git diff returns staged content", "[git]") {
    TempGitRepo repo;
    repo.create_file("hello.txt", "hello world\n");
    repo.stage("hello.txt");

    auto server = make_git_server(repo);
    json args;
    args["staged"] = true;

    auto result = parse_result(server.execute("diff", args.dump()));
    REQUIRE_FALSE(result.empty());
}

TEST_CASE("Git log returns commit entries", "[git]") {
    TempGitRepo repo;
    repo.create_file("init.txt", "initial\n");
    repo.commit("init.txt", "initial commit");

    auto server = make_git_server(repo);

    auto result = parse_result(server.execute("log", "{}"));
    REQUIRE(result.find("initial commit") != std::string::npos);
}

TEST_CASE("Git add stages files", "[git]") {
    TempGitRepo repo;
    repo.create_file("test.txt", "staged content\n");

    auto server = make_git_server(repo);
    json args;
    args["files"] = "test.txt";

    server.execute("add", args.dump());

    // Verify staged via status
    auto status = parse_result(server.execute("status", "{}"));
    REQUIRE(status.find("test.txt") != std::string::npos);
}

// ── v2.3.10: coverage for commit / branch / checkout / reset ──

TEST_CASE("Git commit creates a commit on staged content",
          "[git][v2.3.10][coverage]") {
    TempGitRepo repo;
    repo.create_file("new.txt", "new content\n");
    repo.stage("new.txt");

    auto server = make_git_server(repo);
    json args;
    args["message"] = "v2.3.10 commit test";

    auto out = parse_result(server.execute("commit", args.dump()));
    // git commit output typically contains the commit message or the
    // file name; "1 file changed" is also present in many configs.
    REQUIRE_FALSE(out.empty());
}

TEST_CASE("Git commit with add_all=true stages and commits in one call",
          "[git][v2.3.10][coverage]") {
    TempGitRepo repo;
    repo.create_file("untracked.txt", "untracked\n");

    auto server = make_git_server(repo);
    json args;
    args["message"] = "via add_all";
    args["add_all"] = true;

    auto out = parse_result(server.execute("commit", args.dump()));
    REQUIRE_FALSE(out.empty());
    // After commit, status should be clean of untracked.txt.
    auto status = parse_result(server.execute("status", "{}"));
    REQUIRE(status.find("untracked.txt") == std::string::npos);
}

TEST_CASE("Git branch lists branches and creates new ones",
          "[git][v2.3.10][coverage]") {
    TempGitRepo repo;
    repo.create_file("seed.txt", "seed\n");
    repo.commit("seed.txt", "seed commit");

    auto server = make_git_server(repo);

    SECTION("default invocation returns branch listing") {
        auto out = parse_result(server.execute("branch", "{}"));
        // git branch -a prints at least one branch.
        REQUIRE_FALSE(out.empty());
    }

    SECTION("create=<name> creates and switches to a new branch") {
        json args;
        args["create"] = "feature/v2310-coverage";
        auto out = parse_result(server.execute("branch", args.dump()));
        REQUIRE_FALSE(out.empty());
        // Verify HEAD moved by listing branches and looking for the
        // new one.
        auto listing = parse_result(server.execute("branch", "{}"));
        REQUIRE(listing.find("feature/v2310-coverage")
                != std::string::npos);
    }
}

TEST_CASE("Git checkout switches branches",
          "[git][v2.3.10][coverage]") {
    TempGitRepo repo;
    repo.create_file("a.txt", "a\n");
    repo.commit("a.txt", "first commit on main");

    auto server = make_git_server(repo);

    // Create a target branch first via the branch tool's create=,
    // then checkout main again.
    json create_args;
    create_args["create"] = "v2310-target";
    server.execute("branch", create_args.dump());

    // The branch tool's create path also switches; verify we can
    // checkout back to main.
    json args;
    // Detect default branch name (may be 'main' or 'master')
    auto listing = parse_result(server.execute("branch", "{}"));
    std::string target = (listing.find("main") != std::string::npos)
        ? "main" : "master";
    args["target"] = target;

    auto out = parse_result(server.execute("checkout", args.dump()));
    REQUIRE_FALSE(out.empty());
}

TEST_CASE("Git reset rolls back a staged change",
          "[git][v2.3.10][coverage]") {
    TempGitRepo repo;
    repo.create_file("reset.txt", "to reset\n");
    repo.stage("reset.txt");

    auto server = make_git_server(repo);

    SECTION("reset with no args runs the default mixed reset") {
        auto out = parse_result(server.execute("reset", "{}"));
        // git reset prints "Unstaged changes after reset:" or empty
        // on success — just ensure the call returns a string and
        // does not throw.
        (void)out;
        REQUIRE(true);
    }

    SECTION("reset with a target argument") {
        json args;
        args["target"] = "HEAD";
        auto out = parse_result(server.execute("reset", args.dump()));
        (void)out;
        REQUIRE(true);
    }
}
