// SPDX-License-Identifier: Apache-2.0
/**
 * @file identity_validation_test.cpp
 * @brief gh#211 (v2.14.4): a configured identity that cannot be loaded fails
 *        configure — it never yields an unrestricted tier.
 *
 * A tier's `allowed_tools` lives in its identity's frontmatter. When the
 * identity named in config was missing, or failed to parse, the facade
 * skipped it silently; the tier then had NO allowlist, and the dispatch-time
 * check reads "no allowlist" as unrestricted — so a frontmatter typo handed
 * a read-only tier write_file and edit_file, with nothing logged. A named
 * identity is a requirement, like a named constitution (gh#156): it loads,
 * or configure says why.
 *
 * @version 2.14.4
 */

#include <entropic/prompts/manager.h>
#include <entropic/types/config.h>
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

/// @brief A scratch dir for identity files.
struct Scratch {
    fs::path dir;
    Scratch() {
        dir = fs::temp_directory_path() / ("entropic_idval_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Scratch() { std::error_code ec; fs::remove_all(dir, ec); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

/// @brief A config with one tier whose identity is `path`.
entropic::ParsedConfig with_identity(const fs::path& path) {
    entropic::ParsedConfig cfg;
    entropic::TierConfig tier;
    tier.identity = path;
    cfg.models.tiers["reader"] = tier;
    return cfg;
}

}  // namespace

SCENARIO("gh#211: a configured identity must load, or configure fails",
         "[prompts][identity][security][gh211][2.14.4]") {
    Scratch s;
    const fs::path data_dir = TEST_DATA_DIR;

    WHEN("the named identity file does not exist") {
        auto err = entropic::prompts::validate_configured_prompts(
            with_identity(s.dir / "missing.md"), data_dir);
        THEN("validation fails, naming the tier and the path") {
            INFO(err);
            CHECK(err.find("reader") != std::string::npos);
            CHECK(err.find("missing.md") != std::string::npos);
        }
    }
    WHEN("the named identity's frontmatter does not parse") {
        std::ofstream(s.dir / "broken.md")
            << "---\ntype: identity\nallowed_tools: [unterminated\n---\nbody\n";
        auto err = entropic::prompts::validate_configured_prompts(
            with_identity(s.dir / "broken.md"), data_dir);
        THEN("validation fails rather than dropping the allowlist") {
            INFO(err);
            CHECK(err.find("reader") != std::string::npos);
        }
    }
    WHEN("the named identity is valid") {
        std::ofstream(s.dir / "ok.md")
            << "---\ntype: identity\nversion: 1\nname: reader\n"
               "focus:\n  - read files\n"
               "allowed_tools: [filesystem.read_file]\n---\nYou read files.\n";
        THEN("validation passes") {
            CHECK(entropic::prompts::validate_configured_prompts(
                      with_identity(s.dir / "ok.md"), data_dir).empty());
        }
    }
}

#include <entropic/config/loader.h>

SCENARIO("gh#211: malformed YAML is a returned error, never an abort",
         "[config][yaml][gh211][2.14.4]") {
    // Every ryml parse ran with ryml's default error handler, which aborts
    // the process: one typo in a config file or identity frontmatter took
    // the host down instead of failing configure with a reason.
    entropic::ParsedConfig cfg;
    entropic::config::BundledModels registry;
    const auto err = entropic::config::load_config_from_string(
        "models: [unterminated\n", registry, cfg);
    INFO(err);
    CHECK_FALSE(err.empty());
}
