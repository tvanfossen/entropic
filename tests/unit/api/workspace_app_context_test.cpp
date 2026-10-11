// SPDX-License-Identifier: Apache-2.0
/**
 * @file workspace_app_context_test.cpp
 * @brief gh#179 (v2.14.0): per-workspace app_context, through the C API.
 *
 * One resident model, several repositories: a session bound to a workspace
 * is seeded with that workspace's app_context — set by
 * entropic_workspace_set_app_context, or read (opt-in) from
 * `workspace_app_context` in the workspace root — and every other session
 * keeps the handle's. Read through AgentEngine::session_system_prompt, the
 * exact string a fresh conversation in that session is seeded with.
 *
 * @version 2.14.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>
#include "engine_handle.h"  // white-box: engine->session_system_prompt

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

/// @brief A configured handle plus scratch directories.
struct Fixture {
    fs::path dir;
    entropic_handle_t h = nullptr;
    entropic_error_t rc = ENTROPIC_ERROR_INVALID_STATE;
    explicit Fixture(const std::string& extra = "") {
        static int n = 0;
        dir = fs::temp_directory_path() / ("entropic_wsctx_"
            + std::to_string(::getpid()) + "_" + std::to_string(n++));
        fs::remove_all(dir);
        fs::create_directories(dir);
        entropic_create(&h);
        const std::string cfg = R"({"log_level":"WARN","constitution":false,)"
            R"("app_context":{"content":"HANDLE-CTX"},"mcp":{"working_dir":")"
            + dir.string() + R"("})" + extra + "}";
        rc = entropic_configure(h, cfg.c_str());
    }
    ~Fixture() { entropic_destroy(h); std::error_code ec; fs::remove_all(dir, ec); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    fs::path repo(const std::string& name, const std::string& context = "") const {
        auto root = dir / name;
        fs::create_directories(root / ".entropic");
        if (!context.empty()) { std::ofstream(root / ".entropic" / "ENTROPIC.md") << context; }
        return root;
    }
    std::string prompt(const std::string& session) const {
        return h->engine->session_system_prompt(session);
    }
};

bool has(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

}  // namespace

SCENARIO("gh#179: a workspace's app_context replaces the handle's for its sessions only",
         "[api][workspace][gh179][2.14.0]") {
    Fixture f;
    REQUIRE(f.rc == ENTROPIC_OK);
    const auto a = f.repo("a");
    REQUIRE(entropic_workspace_create(f.h, "A", a.c_str()) == ENTROPIC_OK);
    REQUIRE(entropic_session_bind_workspace(f.h, "in-a", "A") == ENTROPIC_OK);

    WHEN("the workspace is given its own text, after a session was bound") {
        REQUIRE(entropic_workspace_set_app_context(f.h, "A", "WS-A-CTX") == ENTROPIC_OK);
        THEN("the bound session gets it, and an unbound one keeps the handle's") {
            CHECK(has(f.prompt("in-a"), "WS-A-CTX"));
            CHECK_FALSE(has(f.prompt("in-a"), "HANDLE-CTX"));
            CHECK(has(f.prompt("elsewhere"), "HANDLE-CTX"));
        }
        AND_WHEN("the override is dropped with NULL") {
            REQUIRE(entropic_workspace_set_app_context(f.h, "A", nullptr) == ENTROPIC_OK);
            THEN("the session is back on the handle's") {
                CHECK(has(f.prompt("in-a"), "HANDLE-CTX"));
            }
        }
        AND_WHEN("it is set to the empty string") {
            REQUIRE(entropic_workspace_set_app_context(f.h, "A", "") == ENTROPIC_OK);
            THEN("the session has no app_context at all") {
                CHECK_FALSE(has(f.prompt("in-a"), "HANDLE-CTX"));
                CHECK_FALSE(has(f.prompt("in-a"), "WS-A-CTX"));
            }
        }
    }
    THEN("an unknown or missing workspace name is refused") {
        CHECK(entropic_workspace_set_app_context(f.h, "nope", "x")
              == ENTROPIC_ERROR_INVALID_ARGUMENT);
        CHECK(entropic_workspace_set_app_context(f.h, nullptr, "x")
              == ENTROPIC_ERROR_INVALID_ARGUMENT);
        CHECK(entropic_workspace_set_app_context(nullptr, "A", "x")
              == ENTROPIC_ERROR_INVALID_HANDLE);
    }
}

SCENARIO("gh#179: workspace_app_context loads a repository's own file, opt-in",
         "[api][workspace][gh179][2.14.0]") {
    GIVEN("the key set to .entropic/ENTROPIC.md") {
        Fixture f(R"(,"workspace_app_context":".entropic/ENTROPIC.md")");
        REQUIRE(f.rc == ENTROPIC_OK);
        const auto b = f.repo("b", "B-FILE-CTX");
        const auto c = f.repo("c");
        REQUIRE(entropic_workspace_create(f.h, "B", b.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_workspace_create(f.h, "C", c.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(f.h, "in-b", "B") == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(f.h, "in-c", "C") == ENTROPIC_OK);
        THEN("a root with the file uses it; a root without keeps the handle's") {
            CHECK(has(f.prompt("in-b"), "B-FILE-CTX"));
            CHECK_FALSE(has(f.prompt("in-b"), "HANDLE-CTX"));
            CHECK(has(f.prompt("in-c"), "HANDLE-CTX"));
        }
        AND_GIVEN("a repository whose file is a symlink out of its root") {
            const auto d = f.repo("d");
            std::ofstream(f.dir / "secret.txt") << "SECRET";
            fs::create_symlink(f.dir / "secret.txt", d / ".entropic" / "ENTROPIC.md");
            THEN("creating its workspace is refused rather than following it") {
                CHECK(entropic_workspace_create(f.h, "D", d.c_str())
                      == ENTROPIC_ERROR_INVALID_ARGUMENT);
                CHECK(has(entropic_last_error(f.h), "outside the workspace root"));
            }
        }
    }
    GIVEN("the key unset (the default)") {
        Fixture f;
        REQUIRE(f.rc == ENTROPIC_OK);
        const auto b = f.repo("b", "B-FILE-CTX");
        REQUIRE(entropic_workspace_create(f.h, "B", b.c_str()) == ENTROPIC_OK);
        REQUIRE(entropic_session_bind_workspace(f.h, "in-b", "B") == ENTROPIC_OK);
        THEN("the repository's file is ignored") {
            CHECK_FALSE(has(f.prompt("in-b"), "B-FILE-CTX"));
            CHECK(has(f.prompt("in-b"), "HANDLE-CTX"));
        }
    }
}

SCENARIO("gh#179: a workspace_app_context that could leave the root fails configure",
         "[api][workspace][config][gh179][2.14.0]") {
    for (const char* bad : {"../outside.md", "/etc/passwd"}) {
        Fixture f(std::string(R"(,"workspace_app_context":")") + bad + "\"");
        INFO(bad);
        CHECK(f.rc != ENTROPIC_OK);
        CHECK(has(entropic_last_error(f.h), "workspace_app_context"));
    }
}
