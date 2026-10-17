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

SCENARIO("v2.14.5: a grammar-bound tier does not owe an explicit completion",
         "[prompts][identity][grammar][gh212][2.14.5]") {
    // A grammar-constrained tier cannot make a tool call. Defaulting it to
    // explicit_completion nudged every call three times and then failed it —
    // hidden behind ENTROPIC_OK until v2.14.5 reported failed turns.
    entropic::TierConfig tier;
    entropic::prompts::IdentityFrontmatter fm;
    THEN("no grammar, no auto_chain: the contract applies, as before") {
        CHECK(entropic::prompts::derive_explicit_completion(tier, fm));
    }
    THEN("an identity grammar turns the default off") {
        fm.grammar = "npc_verbs";
        CHECK_FALSE(entropic::prompts::derive_explicit_completion(tier, fm));
    }
    THEN("a tier-config grammar turns the default off") {
        tier.grammar = std::filesystem::path("npc.gbnf");
        CHECK_FALSE(entropic::prompts::derive_explicit_completion(tier, fm));
    }
    THEN("an explicit setting still wins") {
        fm.grammar = "npc_verbs";
        fm.explicit_completion = false;
        CHECK_FALSE(entropic::prompts::derive_explicit_completion(tier, fm));
    }
}

SCENARIO("v2.14.5: explicit_completion: true with a grammar fails configure",
         "[prompts][identity][grammar][gh212][2.14.5]") {
    Scratch s;
    const fs::path data_dir = TEST_DATA_DIR;
    std::ofstream(s.dir / "npc.md")
        << "---\ntype: identity\nversion: 1\nname: npc\nfocus:\n  - act\n"
           "grammar: npc_verbs\nexplicit_completion: true\n---\nAct.\n";
    auto err = entropic::prompts::validate_configured_prompts(
        with_identity(s.dir / "npc.md"), data_dir);
    INFO(err);
    CHECK(err.find("reader") != std::string::npos);
    CHECK(err.find("grammar") != std::string::npos);
    CHECK(err.find("explicit_completion") != std::string::npos);
}

SCENARIO("v2.14.5: a config grammar on an identity that owes completion "
         "still configures", "[prompts][identity][grammar][gh154][2.14.5]") {
    // gh#154 requires a tier-config grammar on a bundled agentic identity
    // (which says explicit_completion: true) to configure. Only one FILE
    // stating both is refused; across sources the facade warns.
    Scratch s;
    const fs::path data_dir = TEST_DATA_DIR;
    std::ofstream(s.dir / "agent.md")
        << "---\ntype: identity\nversion: 1\nname: agent\nfocus:\n  - act\n"
           "explicit_completion: true\n---\nAct.\n";
    auto cfg = with_identity(s.dir / "agent.md");
    cfg.models.tiers["reader"].grammar = std::filesystem::path("npc.gbnf");
    CHECK(entropic::prompts::validate_configured_prompts(cfg, data_dir).empty());
}
