// SPDX-License-Identifier: Apache-2.0
/**
 * @file app_context_roundtrip_test.cpp
 * @brief gh#163: `app_context` must reach `ParsedConfig` through every
 *        configure entry point, not just the one the parser tests use.
 *
 * @par Why the C API and not the loader
 * `app_context_content_test.cpp` already pins `parse_config_file`, and
 * `app_context_layering_test.cpp` pins the layered merge. Neither covers the
 * three doors a consumer actually walks through — `entropic_configure`
 * (JSON string), `entropic_configure_from_file` (one YAML file) and
 * `entropic_configure_dir` (layered). The reported symptom was "the key never
 * arrived", and the only way to say which door leaks is to open each one and
 * look at `h->config` behind it.
 *
 * Each case configures against a tier whose GGUF does not exist, so no model
 * is loaded and the return code is irrelevant — `h->config` is populated by
 * the loader before anything expensive runs, and that is what is asserted.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/entropic.h>
#include "engine_handle.h"  // white-box: h->config

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

/// @brief RAII handle that is created but configured by the test itself.
struct RawHandle {
    entropic_handle_t h = nullptr;
    RawHandle() { entropic_create(&h); }
    ~RawHandle() { entropic_destroy(h); }
    operator entropic_handle_t() const { return h; }
};

/// @brief A fresh temp directory for one case.
std::filesystem::path case_dir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path()
        / ("entropic-gh163-api-" + tag);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// @brief Write `body` to `path`.
void write_file(const std::filesystem::path& path, const std::string& body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << body;
}

/// @brief A well-formed app_context prompt file, so gh#156 lets it through.
std::filesystem::path write_app_context(const std::filesystem::path& dir) {
    auto path = dir / "context.md";
    write_file(path, "---\ntype: app_context\nversion: 1\n---\n"
                     "The consumer's document.\n");
    return path;
}

/// @brief YAML models block naming a tier whose GGUF cannot exist.
const char* models_yaml() {
    return "models:\n"
           "  default: lead\n"
           "  lead:\n"
           "    path: /nonexistent/entropic-gh163-api.gguf\n";
}

/// @brief HOME redirect so `configure_dir`'s global layer is deterministic.
struct FakeHome {
    std::string saved;
    bool had_home = false;
    explicit FakeHome(const std::filesystem::path& root) {
        if (const char* h = ::getenv("HOME")) {
            saved = h;
            had_home = true;
        }
        ::setenv("HOME", root.c_str(), 1);
    }
    ~FakeHome() {
        if (had_home) { ::setenv("HOME", saved.c_str(), 1); }
        else { ::unsetenv("HOME"); }
    }
};

}  // namespace

TEST_CASE("gh#163 entropic_configure carries app_context into ParsedConfig",
          "[v2.13.0][entropic_capi][configure][gh163]") {
    auto dir = case_dir("json");
    auto ctx = write_app_context(dir);
    RawHandle h;
    REQUIRE(h.h != nullptr);

    auto cfg = std::string(R"({"models":{"default":"lead",)")
        + R"("lead":{"path":"/nonexistent/entropic-gh163-api.gguf"}},)"
        + R"("app_context":")" + ctx.string() + R"("})";
    entropic_configure(h, cfg.c_str());

    REQUIRE(h.h->config.app_context.has_value());
    CHECK(h.h->config.app_context->string() == ctx.string());
    CHECK_FALSE(h.h->config.app_context_disabled);
}

TEST_CASE("gh#163 entropic_configure_from_file carries app_context",
          "[v2.13.0][entropic_capi][configure][gh163]") {
    auto dir = case_dir("file");
    auto ctx = write_app_context(dir);
    auto cfg = dir / "config.yaml";
    write_file(cfg, std::string(models_yaml())
               + "app_context: \"" + ctx.string() + "\"\n");

    RawHandle h;
    REQUIRE(h.h != nullptr);
    entropic_configure_from_file(h, cfg.c_str());

    REQUIRE(h.h->config.app_context.has_value());
    CHECK(h.h->config.app_context->string() == ctx.string());
}

TEST_CASE("gh#163 entropic_configure_dir carries app_context",
          "[v2.13.0][entropic_capi][configure][gh163]") {
    auto dir = case_dir("dir");
    auto ctx = write_app_context(dir);
    auto project = dir / "project";
    write_file(project / "config.local.yaml",
               std::string(models_yaml())
               + "app_context: \"" + ctx.string() + "\"\n");

    FakeHome home(dir / "home");
    std::filesystem::create_directories(dir / "home");
    RawHandle h;
    REQUIRE(h.h != nullptr);
    entropic_configure_dir(h, project.c_str());

    REQUIRE(h.h->config.app_context.has_value());
    CHECK(h.h->config.app_context->string() == ctx.string());
}

TEST_CASE("gh#163 the object and false spellings survive configure_dir too",
          "[v2.13.0][entropic_capi][configure][gh163]") {
    SECTION("inline content") {
        auto dir = case_dir("dir-inline");
        auto project = dir / "project";
        write_file(project / "config.local.yaml",
                   std::string(models_yaml())
                   + "app_context:\n  content: |\n    Held in memory.\n");
        FakeHome home(dir / "home");
        std::filesystem::create_directories(dir / "home");
        RawHandle h;
        REQUIRE(h.h != nullptr);
        entropic_configure_dir(h, project.c_str());

        REQUIRE(h.h->config.app_context_content.has_value());
        CHECK(h.h->config.app_context_content->find("Held in memory")
              != std::string::npos);
    }

    SECTION("explicit false") {
        auto dir = case_dir("dir-false");
        auto project = dir / "project";
        write_file(project / "config.local.yaml",
                   std::string(models_yaml()) + "app_context: false\n");
        FakeHome home(dir / "home");
        std::filesystem::create_directories(dir / "home");
        RawHandle h;
        REQUIRE(h.h != nullptr);
        entropic_configure_dir(h, project.c_str());

        CHECK(h.h->config.app_context_disabled);
        CHECK_FALSE(h.h->config.app_context.has_value());
    }
}
