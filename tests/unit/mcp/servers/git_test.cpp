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
