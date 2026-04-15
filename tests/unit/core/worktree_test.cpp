// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_worktree.cpp
 * @brief WorktreeManager and ScopedWorktree unit tests.
 * @version 1.8.6
 */

#include <entropic/core/worktree.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace entropic;

// ── Helper ───────────────────────────────────────────────

/**
 * @brief Create a temporary git repository for testing.
 * @return Path to the temporary repo.
 * @internal
 * @version 1.8.6
 */
static std::filesystem::path create_temp_repo() {
    auto tmp = std::filesystem::temp_directory_path() /
               ("entropic_test_" + std::to_string(std::rand()));
    std::filesystem::create_directories(tmp);

    run_git(tmp, "init");
    run_git(tmp, "config user.email 'test@test.com'");
    run_git(tmp, "config user.name 'Test'");

    // Create initial commit
    std::ofstream(tmp / "README.md") << "test\n";
    run_git(tmp, "add -A");
    run_git(tmp, "commit -m 'initial'");

    return tmp;
}

/**
 * @brief Clean up a temporary repo.
 * @param path Repo path to remove.
 * @internal
 * @version 1.8.6
 */
static void cleanup_repo(const std::filesystem::path& path) {
    std::filesystem::remove_all(path);
}

// ── run_git tests ────────────────────────────────────────

TEST_CASE("run_git returns success for valid command",
          "[worktree]") {
    auto repo = create_temp_repo();
    auto r = run_git(repo, "status");
    REQUIRE(r.success);
    REQUIRE_FALSE(r.output.empty());
    cleanup_repo(repo);
}

TEST_CASE("run_git returns failure for invalid command",
          "[worktree]") {
    auto repo = create_temp_repo();
    auto r = run_git(repo, "not-a-command");
    REQUIRE_FALSE(r.success);
    cleanup_repo(repo);
}

// ── WorktreeManager tests ────────────────────────────────

TEST_CASE("ensure_develop creates branch from HEAD",
          "[worktree]") {
    auto repo = create_temp_repo();
    WorktreeManager mgr(repo);

    REQUIRE(mgr.ensure_develop());

    auto r = run_git(repo, "branch --list develop");
    REQUIRE(r.output.find("develop") != std::string::npos);
    cleanup_repo(repo);
}

TEST_CASE("ensure_develop idempotent on second call",
          "[worktree]") {
    auto repo = create_temp_repo();
    WorktreeManager mgr(repo);

    REQUIRE(mgr.ensure_develop());
    REQUIRE(mgr.ensure_develop());
    cleanup_repo(repo);
}

TEST_CASE("create_worktree branches from develop",
          "[worktree]") {
    auto repo = create_temp_repo();
    WorktreeManager mgr(repo);
    mgr.ensure_develop();

    auto wt = mgr.create_worktree("abc12345", "eng");
    REQUIRE(wt.has_value());
    REQUIRE(wt->branch == "delegation/eng-abc12345");
    REQUIRE(std::filesystem::exists(wt->path));

    mgr.discard_worktree(*wt);
    cleanup_repo(repo);
}

TEST_CASE("create_worktree fails without develop",
          "[worktree]") {
    auto repo = create_temp_repo();
    WorktreeManager mgr(repo);
    // Don't call ensure_develop()

    auto wt = mgr.create_worktree("abc12345", "eng");
    REQUIRE_FALSE(wt.has_value());
    cleanup_repo(repo);
}

TEST_CASE("merge_worktree merges changes to develop",
          "[worktree]") {
    auto repo = create_temp_repo();
    WorktreeManager mgr(repo);
    mgr.ensure_develop();

    auto wt = mgr.create_worktree("merge01", "eng");
    REQUIRE(wt.has_value());

    // Create a file in the worktree
    std::ofstream(wt->path / "new_file.txt") << "content\n";
    run_git(wt->path, "add -A");
    run_git(wt->path, "commit -m 'add file'");

    REQUIRE(mgr.merge_worktree(*wt));

    // File should exist on develop
    REQUIRE(std::filesystem::exists(repo / "new_file.txt"));
    cleanup_repo(repo);
}

TEST_CASE("merge_worktree auto-commits dirty changes",
          "[worktree]") {
    auto repo = create_temp_repo();
    WorktreeManager mgr(repo);
    mgr.ensure_develop();

    auto wt = mgr.create_worktree("dirty01", "eng");
    REQUIRE(wt.has_value());

    // Create file but don't commit
    std::ofstream(wt->path / "dirty.txt") << "uncommitted\n";

    REQUIRE(mgr.merge_worktree(*wt));

    // File should exist after auto-commit + merge
    REQUIRE(std::filesystem::exists(repo / "dirty.txt"));
    cleanup_repo(repo);
}

TEST_CASE("discard_worktree cleans up without merge",
          "[worktree]") {
    auto repo = create_temp_repo();
    WorktreeManager mgr(repo);
    mgr.ensure_develop();

    auto wt = mgr.create_worktree("discard01", "qa");
    REQUIRE(wt.has_value());
    auto wt_path = wt->path;

    mgr.discard_worktree(*wt);

    REQUIRE_FALSE(std::filesystem::exists(wt_path));
    cleanup_repo(repo);
}

TEST_CASE("worktree directory naming convention",
          "[worktree]") {
    auto repo = create_temp_repo();
    WorktreeManager mgr(repo);
    mgr.ensure_develop();

    auto wt = mgr.create_worktree("x1y2z3a4", "eng");
    REQUIRE(wt.has_value());

    // Path should be .worktrees/delegation-<short_id>
    REQUIRE(wt->path.filename().string() == "delegation-x1y2z3a4");

    mgr.discard_worktree(*wt);
    cleanup_repo(repo);
}

// ── ScopedWorktree tests ─────────────────────────────────

TEST_CASE("ScopedWorktree swaps and restores directory",
          "[worktree]") {
    std::filesystem::path swapped_to;
    int swap_count = 0;

    auto swap_fn = [](const std::filesystem::path& path,
                      void* ud) {
        auto* state = static_cast<
            std::pair<std::filesystem::path*, int*>*>(ud);
        *state->first = path;
        (*state->second)++;
    };

    std::pair<std::filesystem::path*, int*> state{
        &swapped_to, &swap_count};

    auto wt_path = std::filesystem::path("/tmp/wt");
    auto orig_path = std::filesystem::path("/project");

    {
        ScopedWorktree scope(swap_fn, &state, wt_path, orig_path);
        REQUIRE(swapped_to == wt_path);
        REQUIRE(swap_count == 1);
    }

    // Destructor should restore
    REQUIRE(swapped_to == orig_path);
    REQUIRE(swap_count == 2);
}
