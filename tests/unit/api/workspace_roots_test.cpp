// SPDX-License-Identifier: Apache-2.0
/**
 * @file workspace_roots_test.cpp
 * @brief gh#211 (v2.14.4): `mcp.workspace_roots` bounds which directories
 *        may become a workspace.
 *
 * A workspace root is a tool root: the filesystem tools are confined to it,
 * and declared servers launch in it. validate_workspace_args checked only
 * that the directory existed, and a bridge client names workspaces by
 * absolute path — so `workspace: "/"` (or `$HOME`) made the model a reader
 * of the whole account. With `workspace_roots` set, a root must resolve —
 * symlinks followed — inside one of the listed directories.
 *
 * @version 2.14.4
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>

#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

/// @brief A handle configured with `mcp.workspace_roots` (JSON array text).
struct RootsHandle {
    entropic_handle_t h = nullptr;
    entropic_error_t rc = ENTROPIC_ERROR_INVALID_STATE;
    fs::path dir;
    explicit RootsHandle(const std::string& roots_json) {
        dir = fs::temp_directory_path() / ("entropic_wsroots_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir / "allowed" / "repo");
        fs::create_directories(dir / "other");
        entropic_create(&h);
        const std::string cfg = R"({"log_level":"WARN","mcp":{"working_dir":")"
            + dir.string() + R"(","workspace_roots":)" + roots_json + "}}";
        rc = entropic_configure(h, cfg.c_str());
    }
    ~RootsHandle() { entropic_destroy(h); std::error_code ec; fs::remove_all(dir, ec); }
    RootsHandle(const RootsHandle&) = delete;
    RootsHandle& operator=(const RootsHandle&) = delete;
};

bool mentions(entropic_handle_t h, const char* needle) {
    const char* e = entropic_last_error(h);
    return e != nullptr && std::string(e).find(needle) != std::string::npos;
}

}  // namespace

SCENARIO("gh#211: workspace_roots admits only directories inside it",
         "[api][workspace][security][gh211][2.14.4]") {
    const auto base = fs::temp_directory_path() / ("entropic_wsroots_" + std::to_string(::getpid()));
    RootsHandle r("[\"" + (base / "allowed").string() + "\"]");
    REQUIRE(r.rc == ENTROPIC_OK);

    THEN("a repository under an allowed root is accepted") {
        CHECK(entropic_workspace_create(r.h, "repo", (r.dir / "allowed" / "repo").c_str())
              == ENTROPIC_OK);
    }
    THEN("a directory outside every root is refused, naming the setting") {
        CHECK(entropic_workspace_create(r.h, "other", (r.dir / "other").c_str())
              == ENTROPIC_ERROR_INVALID_ARGUMENT);
        CHECK(mentions(r.h, "workspace_roots"));
        CHECK(entropic_workspace_create(r.h, "slash", "/") == ENTROPIC_ERROR_INVALID_ARGUMENT);
    }
    THEN("a symlink inside an allowed root that leads outside is refused") {
        fs::create_directory_symlink(r.dir / "other", r.dir / "allowed" / "escape");
        CHECK(entropic_workspace_create(r.h, "escape", (r.dir / "allowed" / "escape").c_str())
              == ENTROPIC_ERROR_INVALID_ARGUMENT);
    }
    THEN("a `..` that climbs out is refused") {
        const auto climb = (r.dir / "allowed" / ".." / "other").string();
        CHECK(entropic_workspace_create(r.h, "climb", climb.c_str())
              == ENTROPIC_ERROR_INVALID_ARGUMENT);
    }
}

SCENARIO("gh#211: unset workspace_roots keeps today's behaviour; a relative "
         "entry fails configure", "[api][workspace][security][gh211][2.14.4]") {
    GIVEN("no restriction") {
        RootsHandle r("[]");
        REQUIRE(r.rc == ENTROPIC_OK);
        CHECK(entropic_workspace_create(r.h, "other", (r.dir / "other").c_str()) == ENTROPIC_OK);
    }
    GIVEN("a relative entry") {
        RootsHandle r("[\"relative/dir\"]");
        CHECK(r.rc != ENTROPIC_OK);
        CHECK(mentions(r.h, "workspace_roots"));
    }
}
