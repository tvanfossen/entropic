// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file workspace_test.cpp
 * @brief gh#166 (v2.13.0): named workspaces — one resident model, many
 *        repositories.
 *
 * Before this, `mcp.working_dir` was fixed at configure time, so serving
 * a second repository meant a second handle and a second copy of the
 * weights. These tests pin the three claims that make one handle safe to
 * share: two workspaces resolve the SAME relative path to two different
 * roots, a bound session cannot reach the other workspace's files, and a
 * session that is never bound behaves exactly as it did before.
 *
 * White-box (`engine_handle.h`) for the routing decision, because the
 * alternative — running a turn — needs a model and would test the
 * sampler as much as the routing.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>
#include "engine_handle.h"  // white-box: workspace_servers(), h->engine

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

/**
 * @brief RAII guard that creates AND configures a handle.
 * @internal
 * @version 2.13.0
 */
struct WsHandle {
    entropic_handle_t h = nullptr;  ///< Handle under test
    bool ok = false;                ///< Whether configure succeeded

    WsHandle() {
        entropic_create(&h);
        if (h != nullptr) {
            ok = entropic_configure(h, R"({"log_level":"WARN"})")
                 == ENTROPIC_OK;
        }
    }
    ~WsHandle() { entropic_destroy(h); }
    operator entropic_handle_t() const { return h; }  // NOLINT
};

/**
 * @brief Make a temp project holding one marker file.
 * @param tag Distinctive marker written into notes.md.
 * @return Path to the created directory.
 * @internal
 * @version 2.13.0
 */
fs::path make_repo(const std::string& tag) {
    auto dir = fs::temp_directory_path() /
               ("entropic_ws_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "notes.md") << "MARKER-" << tag << "\n";
    return dir;
}

}  // namespace

SCENARIO("gh#166: two workspaces resolve one relative path to two roots",
         "[api][workspace][gh166][v2.13.0]") {
    WsHandle h;
    REQUIRE(h.ok);

    auto repo_a = make_repo("alpha");
    auto repo_b = make_repo("bravo");

    GIVEN("two workspaces and one session bound to each") {
        REQUIRE(entropic_workspace_create(h, "proj-a",
                                          repo_a.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_workspace_create(h, "proj-b",
                                          repo_b.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(h, "sa", "proj-a")
                == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(h, "sb", "proj-b")
                == ENTROPIC_OK);

        auto* sa = entropic::workspace_servers(h, "sa");
        auto* sb = entropic::workspace_servers(h, "sb");

        THEN("each session gets its OWN server instances") {
            // A server holds one working directory, so sharing one set
            // between two repositories is not a thing that can work.
            REQUIRE(sa != nullptr);
            REQUIRE(sb != nullptr);
            CHECK(sa != sb);
            CHECK(sa != h.h->server_manager.get());
            CHECK(fs::path(sa->project_dir()) == repo_a);
            CHECK(fs::path(sb->project_dir()) == repo_b);
        }

        THEN("the same relative path reads two different files") {
            auto ra = sa->execute("filesystem.read_file",
                                  R"({"path":"notes.md"})");
            auto rb = sb->execute("filesystem.read_file",
                                  R"({"path":"notes.md"})");
            INFO("A: " << ra << "\nB: " << rb);
            CHECK(ra.find("MARKER-alpha") != std::string::npos);
            CHECK(rb.find("MARKER-bravo") != std::string::npos);
            // ...and neither can see the other's copy.
            CHECK(ra.find("MARKER-bravo") == std::string::npos);
            CHECK(rb.find("MARKER-alpha") == std::string::npos);
        }

        THEN("a bound session cannot escape into the other workspace") {
            // An absolute path into B is refused by A's root-confinement,
            // which is per-server and therefore per-workspace.
            auto escape = sa->execute(
                "filesystem.read_file",
                R"({"path":")" + (repo_b / "notes.md").string() + R"("})");
            INFO("escape attempt: " << escape);
            CHECK(escape.find("MARKER-bravo") == std::string::npos);
        }

        THEN("the delegation sandbox roots at the session's workspace") {
            // gh#160's single seam, re-pointed — nothing else in the
            // delegation path had to change.
            CHECK(h.h->engine->resolve_session_root("sa") == repo_a);
            CHECK(h.h->engine->resolve_session_root("sb") == repo_b);
        }

        THEN("an UNBOUND session still uses the handle's default set") {
            CHECK(entropic::workspace_servers(h, "")
                  == h.h->server_manager.get());
            CHECK(entropic::workspace_servers(h, "never-bound")
                  == h.h->server_manager.get());
            CHECK(entropic::workspace_for(h, "") == nullptr);
        }
    }

    fs::remove_all(repo_a);
    fs::remove_all(repo_b);
}

SCENARIO("gh#166: rebinding a session that already has messages is refused",
         "[api][workspace][gh166][v2.13.0]") {
    WsHandle h;
    REQUIRE(h.ok);
    auto repo_a = make_repo("rebind_a");
    auto repo_b = make_repo("rebind_b");

    REQUIRE(entropic_workspace_create(h, "ws-a", repo_a.c_str())
            == ENTROPIC_OK);
    REQUIRE(entropic_workspace_create(h, "ws-b", repo_b.c_str())
            == ENTROPIC_OK);

    GIVEN("a session bound before its first turn") {
        REQUIRE(entropic_session_bind_workspace(h, "s1", "ws-a")
                == ENTROPIC_OK);

        WHEN("it is rebound while still empty") {
            THEN("that is allowed — nothing cites a path yet") {
                CHECK(entropic_session_bind_workspace(h, "s1", "ws-b")
                      == ENTROPIC_OK);
            }
        }

        WHEN("the session has accumulated messages") {
            REQUIRE(entropic_session_context_set(
                        h, "s1",
                        R"([{"role":"user","content":"read src/a.cpp"}])")
                    == ENTROPIC_OK);
            THEN("rebinding is refused with a typed error") {
                // A conversation cites paths relative to the root it was
                // built in; re-rooting it silently would make every one
                // of those citations wrong with no error anywhere.
                auto rc = entropic_session_bind_workspace(h, "s1", "ws-b");
                CHECK(rc == ENTROPIC_ERROR_INVALID_STATE);
                CHECK(std::string(entropic_last_error(h))
                          .find("already holds messages")
                      != std::string::npos);
                // ...and the binding it had is untouched. (Catch2 runs
                // each WHEN from a fresh GIVEN, so "s1" is still on ws-a
                // here — which is exactly the point: the refusal left it
                // where it was.)
                CHECK(fs::path(entropic::workspace_servers(h, "s1")
                                   ->project_dir())
                      == repo_a);
            }
        }
    }

    fs::remove_all(repo_a);
    fs::remove_all(repo_b);
}

SCENARIO("gh#166: workspace API guards are typed",
         "[api][workspace][gh166][v2.13.0]") {
    GIVEN("a null handle") {
        CHECK(entropic_workspace_create(nullptr, "w", "/tmp")
              == ENTROPIC_ERROR_INVALID_HANDLE);
        CHECK(entropic_session_bind_workspace(nullptr, "s", "w")
              == ENTROPIC_ERROR_INVALID_HANDLE);
    }

    GIVEN("an unconfigured handle") {
        entropic_handle_t h = nullptr;
        entropic_create(&h);
        CHECK(entropic_workspace_create(h, "w", "/tmp")
              == ENTROPIC_ERROR_INVALID_STATE);
        CHECK(entropic_session_bind_workspace(h, "s", "w")
              == ENTROPIC_ERROR_INVALID_STATE);
        entropic_destroy(h);
    }

    GIVEN("a configured handle") {
        WsHandle h;
        REQUIRE(h.ok);
        auto repo = make_repo("guards");

        THEN("empty names and non-directories are refused") {
            CHECK(entropic_workspace_create(h, "", repo.c_str())
                  == ENTROPIC_ERROR_INVALID_ARGUMENT);
            CHECK(entropic_workspace_create(h, "w", nullptr)
                  == ENTROPIC_ERROR_INVALID_ARGUMENT);
            CHECK(entropic_workspace_create(
                      h, "w", (repo / "notes.md").c_str())
                  == ENTROPIC_ERROR_INVALID_ARGUMENT);
        }
        THEN("a duplicate name is refused rather than replacing servers") {
            REQUIRE(entropic_workspace_create(h, "dup", repo.c_str())
                    == ENTROPIC_OK);
            CHECK(entropic_workspace_create(h, "dup", repo.c_str())
                  == ENTROPIC_ERROR_INVALID_ARGUMENT);
        }
        THEN("binding to an unknown workspace is refused") {
            CHECK(entropic_session_bind_workspace(h, "s", "nope")
                  == ENTROPIC_ERROR_INVALID_ARGUMENT);
            CHECK(entropic::workspace_servers(h, "s")
                  == h.h->server_manager.get());
        }
        fs::remove_all(repo);
    }
}
