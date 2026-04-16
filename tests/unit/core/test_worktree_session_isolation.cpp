// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_worktree_session_isolation.cpp
 * @brief Regression tests for v2.0.6 worktree fixes.
 *
 * Fix 2: orphan `.worktrees/delegation-*` dirs are removed at startup.
 * Fix 3: `.worktrees/<pid>-<hex>/` session scoping + dead-PID pruning.
 *
 * @version 2.0.6
 */

#include <entropic/core/worktree.h>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>

#include <unistd.h>

using namespace entropic;
namespace fs = std::filesystem;

// ── Helper ──────────────────────────────────────────────

/**
 * @brief Create a temporary git repository for testing.
 *
 * Uses pid + random_device to avoid collisions when ctest
 * launches multiple test processes in parallel.
 *
 * @return Path to the temporary repo.
 * @internal
 * @version 2.0.6
 */
static fs::path create_temp_repo() {
    std::random_device rd;
    auto suffix = std::to_string(getpid()) + "_"
                + std::to_string(rd());
    auto tmp = fs::temp_directory_path() /
               ("entropic_wt_" + suffix);
    fs::create_directories(tmp);
    run_git(tmp, "init");
    run_git(tmp, "config user.email 'test@test.com'");
    run_git(tmp, "config user.name 'Test'");
    std::ofstream(tmp / "README.md") << "test\n";
    run_git(tmp, "add -A");
    run_git(tmp, "commit -m 'initial'");
    return tmp;
}

/**
 * @brief Clean up a temporary repo.
 * @param path Repo path to remove.
 * @internal
 * @version 2.0.6
 */
static void cleanup_repo(const fs::path& path) {
    fs::remove_all(path);
}

// ── Fix 2: orphan directory cleanup ─────────────────────

SCENARIO("Pre-v2.0.6 orphan delegation dir is pruned at construction",
         "[worktree][regression][v2.0.6]")
{
    GIVEN("a repo with a fake .worktrees/delegation-zzzz/ dir") {
        auto repo = create_temp_repo();
        auto orphan = repo / ".worktrees" / "delegation-zzzz";
        fs::create_directories(orphan);
        REQUIRE(fs::exists(orphan));

        WHEN("a WorktreeManager is constructed") {
            { WorktreeManager mgr(repo); }

            THEN("the orphan directory is removed") {
                CHECK_FALSE(fs::exists(orphan));
            }
        }
        cleanup_repo(repo);
    }
}

// ── Fix 3: session-scoped worktree paths ────────────────

SCENARIO("Two managers get distinct session IDs",
         "[worktree][v2.0.6]")
{
    GIVEN("two WorktreeManagers on the same repo") {
        auto repo = create_temp_repo();

        WorktreeManager mgr1(repo);
        WorktreeManager mgr2(repo);

        THEN("they have different session-scoped bases") {
            // Both create subdirs under .worktrees/; neither should
            // collide. We verify by checking that .worktrees/ has
            // at least 2 subdirectories.
            auto base = repo / ".worktrees";
            int subdirs = 0;
            for (const auto& e : fs::directory_iterator(base)) {
                if (e.is_directory()) { subdirs++; }
            }
            CHECK(subdirs >= 2);
        }
        cleanup_repo(repo);
    }
}

SCENARIO("Dead-PID session dir is pruned at startup",
         "[worktree][regression][v2.0.6]")
{
    GIVEN("a repo with a fake .worktrees/99999999-aabbccdd/ dir") {
        auto repo = create_temp_repo();
        // PID 99999999 is extremely unlikely to be alive.
        auto dead_session = repo / ".worktrees" / "99999999-aabbccdd";
        fs::create_directories(dead_session);
        REQUIRE(fs::exists(dead_session));

        WHEN("a WorktreeManager is constructed") {
            { WorktreeManager mgr(repo); }

            THEN("the dead-PID session dir is removed") {
                CHECK_FALSE(fs::exists(dead_session));
            }
        }
        cleanup_repo(repo);
    }
}

SCENARIO("Current-session dir survives prune",
         "[worktree][v2.0.6]")
{
    GIVEN("a WorktreeManager with its own session dir") {
        auto repo = create_temp_repo();
        WorktreeManager mgr(repo);

        // .worktrees/<session_id>/ should exist
        auto base = repo / ".worktrees";
        int session_dirs = 0;
        for (const auto& e : fs::directory_iterator(base)) {
            if (e.is_directory()) { session_dirs++; }
        }

        THEN("at least one session dir exists after construction") {
            CHECK(session_dirs >= 1);
        }
        cleanup_repo(repo);
    }
}
