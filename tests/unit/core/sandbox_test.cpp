// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file sandbox_test.cpp
 * @brief SandboxManager and ScopedSandbox unit tests (gh#29, v2.1.5).
 *
 * The critical invariant under test: the user's project directory is
 * never modified by SandboxManager operations. Every test asserts the
 * project tree is byte-identical before and after the operation.
 *
 * @version 2.1.5
 */

#include <entropic/core/sandbox.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <unistd.h>

using namespace entropic;
namespace fs = std::filesystem;

// ── Helpers ──────────────────────────────────────────────

/**
 * @brief Create a temporary project tree (git or plain).
 * @param git If true, run `git init` in the new dir.
 * @return Path to the temporary project.
 * @internal
 * @version 2.1.5
 */
static fs::path make_temp_project(bool git) {
    auto tmp = fs::temp_directory_path() /
               ("entropic_sb_" + std::to_string(getpid()) + "_" +
                std::to_string(std::rand()));
    fs::create_directories(tmp);
    {
        std::ofstream(tmp / "hello.txt") << "hello\n";
        fs::create_directories(tmp / "src");
        std::ofstream(tmp / "src" / "main.cpp") << "int main(){return 0;}\n";
    }
    if (git) {
        std::string init = "cd '" + tmp.string() +
                           "' && git init -q && "
                           "git config user.email a@b && "
                           "git config user.name a && "
                           "git add -A && git commit -qm initial";
        std::system(init.c_str());
    }
    return tmp;
}

/**
 * @brief Read every regular file under `root` into a {relpath → contents} map.
 * @param root Directory to walk.
 * @return Map for byte-level comparison.
 * @internal
 * @version 2.1.5
 */
static std::map<std::string, std::string> snapshot_tree(const fs::path& root) {
    std::map<std::string, std::string> out;
    if (!fs::exists(root)) { return out; }
    for (auto& p : fs::recursive_directory_iterator(root)) {
        if (!p.is_regular_file()) { continue; }
        auto rel = fs::relative(p.path(), root).string();
        if (rel.rfind(".git", 0) == 0) { continue; } // ignore git internals
        std::ifstream in(p.path(), std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        out[rel] = std::move(content);
    }
    return out;
}

/**
 * @brief Override `$HOME` for the duration of a test.
 * @internal
 * @version 2.1.5
 */
struct ScopedHome {
    std::string original;
    fs::path tmp_home;
    ScopedHome() {
        const char* h = std::getenv("HOME");
        original = h ? h : "";
        tmp_home = fs::temp_directory_path() /
                   ("entropic_home_" + std::to_string(getpid()) + "_" +
                    std::to_string(std::rand()));
        fs::create_directories(tmp_home);
        setenv("HOME", tmp_home.string().c_str(), 1);
    }
    ~ScopedHome() {
        if (!original.empty()) {
            setenv("HOME", original.c_str(), 1);
        } else {
            unsetenv("HOME");
        }
        std::error_code ec;
        fs::remove_all(tmp_home, ec);
    }
};

// ── Constructor / destructor ─────────────────────────────

TEST_CASE("SandboxManager creates session dir under $HOME/.entropic/sandbox",
          "[sandbox]") {
    ScopedHome home;
    auto project = make_temp_project(false);
    {
        SandboxManager mgr(project);
        REQUIRE(fs::exists(mgr.session_base()));
        REQUIRE(mgr.session_base().string().find(
            (home.tmp_home / ".entropic" / "sandbox").string()) == 0);
    }
    fs::remove_all(project);
}

TEST_CASE("SandboxManager destructor removes the session sandbox tree",
          "[sandbox]") {
    ScopedHome home;
    auto project = make_temp_project(false);
    fs::path session_base;
    {
        SandboxManager mgr(project);
        session_base = mgr.session_base();
        auto info = mgr.create_sandbox("d1");
        REQUIRE(info.has_value());
        REQUIRE(fs::exists(info->path));
    }
    REQUIRE_FALSE(fs::exists(session_base));
    fs::remove_all(project);
}

// ── Project repo untouched (the gh#29 invariant) ─────────

TEST_CASE("SandboxManager never modifies the user project (git source)",
          "[sandbox][critical]") {
    ScopedHome home;
    auto project = make_temp_project(true);
    auto before = snapshot_tree(project);
    {
        SandboxManager mgr(project);
        auto sb = mgr.create_sandbox("d1");
        REQUIRE(sb.has_value());
        std::ofstream(sb->path / "hello.txt") << "edited\n";
        auto result = mgr.finalize_sandbox(*sb);
        REQUIRE(result.has_value());
        REQUIRE(result->patch.find("edited") != std::string::npos);
        mgr.discard_sandbox(*sb);
    }
    auto after = snapshot_tree(project);
    REQUIRE(before == after);
    fs::remove_all(project);
}

TEST_CASE("SandboxManager never modifies the user project (plain source)",
          "[sandbox][critical]") {
    ScopedHome home;
    auto project = make_temp_project(false);
    auto before = snapshot_tree(project);
    {
        SandboxManager mgr(project);
        auto sb = mgr.create_sandbox("d1");
        REQUIRE(sb.has_value());
        std::ofstream(sb->path / "src" / "main.cpp") << "// edited\n";
        auto result = mgr.finalize_sandbox(*sb);
        REQUIRE(result.has_value());
        mgr.discard_sandbox(*sb);
    }
    REQUIRE(before == snapshot_tree(project));
    fs::remove_all(project);
}

// ── Snapshot fidelity ────────────────────────────────────

TEST_CASE("create_sandbox copies project files to sandbox (git source)",
          "[sandbox]") {
    ScopedHome home;
    auto project = make_temp_project(true);
    SandboxManager mgr(project);
    auto sb = mgr.create_sandbox("d1");
    REQUIRE(sb.has_value());
    REQUIRE(fs::exists(sb->path / "hello.txt"));
    REQUIRE(fs::exists(sb->path / "src" / "main.cpp"));
    fs::remove_all(project);
}

TEST_CASE("create_sandbox honors .gitignore on git sources", "[sandbox]") {
    ScopedHome home;
    auto project = make_temp_project(true);
    std::ofstream(project / ".gitignore") << "ignored.txt\n";
    std::ofstream(project / "ignored.txt") << "should be skipped\n";
    std::string add = "cd '" + project.string() +
                      "' && git add .gitignore && "
                      "git commit -qm gi";
    std::system(add.c_str());

    SandboxManager mgr(project);
    auto sb = mgr.create_sandbox("d1");
    REQUIRE(sb.has_value());
    REQUIRE(fs::exists(sb->path / "hello.txt"));
    REQUIRE_FALSE(fs::exists(sb->path / "ignored.txt"));
    fs::remove_all(project);
}

// ── Pipeline chaining ────────────────────────────────────

TEST_CASE("create_sandbox chains forward-carry from a prior sandbox",
          "[sandbox][pipeline]") {
    ScopedHome home;
    auto project = make_temp_project(false);
    SandboxManager mgr(project);

    auto s1 = mgr.create_sandbox("d1");
    REQUIRE(s1.has_value());
    std::ofstream(s1->path / "new_from_d1.txt") << "from d1\n";

    auto s2 = mgr.create_sandbox("d2", s1);
    REQUIRE(s2.has_value());
    REQUIRE(fs::exists(s2->path / "new_from_d1.txt"));
    REQUIRE(s2->base_dir == s1->base_dir);
    fs::remove_all(project);
}

// ── Patch generation ─────────────────────────────────────

TEST_CASE("finalize_sandbox returns empty patch when nothing changed",
          "[sandbox]") {
    ScopedHome home;
    auto project = make_temp_project(false);
    SandboxManager mgr(project);
    auto sb = mgr.create_sandbox("d1");
    REQUIRE(sb.has_value());
    auto result = mgr.finalize_sandbox(*sb);
    REQUIRE(result.has_value());
    REQUIRE(result->files_touched.empty());
    REQUIRE(result->patch.empty());
    fs::remove_all(project);
}

TEST_CASE("finalize_sandbox produces a unified diff for edited files",
          "[sandbox]") {
    ScopedHome home;
    auto project = make_temp_project(false);
    SandboxManager mgr(project);
    auto sb = mgr.create_sandbox("d1");
    REQUIRE(sb.has_value());
    std::ofstream(sb->path / "hello.txt") << "edited contents\n";
    auto result = mgr.finalize_sandbox(*sb);
    REQUIRE(result.has_value());
    REQUIRE(result->files_touched.size() == 1);
    REQUIRE(result->patch.find("+edited contents") != std::string::npos);
    REQUIRE(result->patch.find("-hello") != std::string::npos);
    fs::remove_all(project);
}

// ── Path containment ─────────────────────────────────────

TEST_CASE("discard_sandbox refuses paths outside session base",
          "[sandbox][safety]") {
    ScopedHome home;
    auto project = make_temp_project(false);
    SandboxManager mgr(project);

    auto bogus_dir = fs::temp_directory_path() /
                     ("entropic_outside_" +
                      std::to_string(std::rand()));
    fs::create_directories(bogus_dir);
    std::ofstream(bogus_dir / "important.txt") << "must not be deleted\n";

    SandboxInfo bogus{bogus_dir, "evil", mgr.session_base() / "base"};
    mgr.discard_sandbox(bogus);

    REQUIRE(fs::exists(bogus_dir / "important.txt"));
    fs::remove_all(bogus_dir);
    fs::remove_all(project);
}

// ── ScopedSandbox ────────────────────────────────────────

namespace {
fs::path g_swap_observed;
fs::path g_restore_observed;
int g_swap_call_count = 0;

void test_swap(const fs::path& path, void* user_data) {
    (void)user_data;
    if (g_swap_call_count == 0) {
        g_swap_observed = path;
    } else {
        g_restore_observed = path;
    }
    ++g_swap_call_count;
}
} // namespace

TEST_CASE("ScopedSandbox swaps on construct and restores on destruct",
          "[sandbox][scoped]") {
    g_swap_observed.clear();
    g_restore_observed.clear();
    g_swap_call_count = 0;

    fs::path sandbox_path = "/tmp/sandbox_path";
    fs::path original = "/tmp/original";
    {
        ScopedSandbox s(test_swap, nullptr, sandbox_path, original);
        REQUIRE(g_swap_observed == sandbox_path);
        REQUIRE(g_swap_call_count == 1);
    }
    REQUIRE(g_restore_observed == original);
    REQUIRE(g_swap_call_count == 2);
}

TEST_CASE("ScopedSandbox no-op when swap_fn is null", "[sandbox][scoped]") {
    fs::path p = "/tmp/x";
    ScopedSandbox s(nullptr, nullptr, p, p);
    SUCCEED();
}

// ── gh#33 (v2.1.6): session-scoped lifecycle ─────────────

/**
 * @brief A session-scoped SandboxManager preserves session_base_ across
 *        multiple create_sandbox / discard cycles.
 *
 * Regression for gh#33: pre-2.1.6 each delegation rebuilt the manager,
 * which destroyed and recreated `session_base_` on every call. With the
 * v2.1.6 lifecycle a single manager handles many delegations; this test
 * asserts that the session directory survives a discard+create round-trip.
 *
 * @version 2.1.6
 */
TEST_CASE("SandboxManager survives discard+create cycle", "[sandbox][gh33][v2.1.6]") {
    ScopedHome home;
    auto project = home.tmp_home / "proj";
    fs::create_directories(project);
    std::ofstream(project / "a.txt") << "hello\n";

    SandboxManager mgr(project);
    auto session_root = mgr.session_base();

    auto s1 = mgr.create_sandbox("d1");
    REQUIRE(s1.has_value());
    REQUIRE(fs::exists(s1->path));
    mgr.discard_sandbox(*s1);
    REQUIRE_FALSE(fs::exists(s1->path));
    // Critical: session_base_ must still exist (the manager is alive).
    REQUIRE(fs::exists(session_root));

    auto s2 = mgr.create_sandbox("d2");
    REQUIRE(s2.has_value());
    REQUIRE(fs::exists(s2->path));
    // Same session, different delegation dir.
    REQUIRE(s2->path.parent_path() == session_root);
    REQUIRE(s1->path != s2->path);
    mgr.discard_sandbox(*s2);
}
