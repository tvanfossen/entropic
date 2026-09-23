// SPDX-License-Identifier: Apache-2.0
/**
 * @file app_context_layering_test.cpp
 * @brief gh#163: does a top-level `app_context: "<abs path>"` survive the
 *        config layers, and can a consumer tell the three no-body states apart?
 *
 * @par What was reported
 * A configured path produced `App context disabled (not configured)` in the
 * log. That line (`load_app_context`) is only reachable when the parsed
 * `app_context_path` is `nullopt`, while `extract_tri_state_path` maps any
 * string to a path — so the report reads as "the key never reached
 * `ParsedConfig`", which would be a layering defect.
 *
 * @par Two separable claims
 * The first is a ROUND-TRIP claim and is settled by evidence: every layer
 * below is exercised with the key set, overridden, disabled and supplied
 * inline. Whatever these cases say is what the merge actually does; they
 * exist so the question never has to be re-argued from reading `loader.cpp`.
 *
 * The second is a DIAGNOSTIC claim and stands on its own: one log line served
 * three distinguishable states — not configured, explicitly disabled, and
 * configured-but-rejected — so the message could not tell a consumer which
 * one they were in even when the engine knew. That is fixed here regardless
 * of what the round-trip cases show.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/config/bundled_models.h>
#include <entropic/config/loader.h>
#include <entropic/prompts/manager.h>
#include <entropic/types/logging.h>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

/// @brief Write `body` to `path`, creating parents.
void write_file(const std::filesystem::path& path, const std::string& body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << body;
}

/**
 * @brief A throwaway HOME whose `~/.entropic/config.yaml` the test owns.
 *
 * `load_global_layer` reads $HOME directly, so the global layer cannot be
 * exercised without redirecting it. HOME is restored on destruction so the
 * cases stay independent when the binary is run without a ctest filter.
 */
struct FakeHome {
    std::filesystem::path root;
    std::string saved;
    bool had_home = false;

    explicit FakeHome(const std::string& tag) {
        root = std::filesystem::temp_directory_path()
            / ("entropic-gh163-" + tag);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
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

    /// @brief Write the global layer (`~/.entropic/config.yaml`).
    void global(const std::string& body) const {
        write_file(root / ".entropic" / "config.yaml", body);
    }
    /// @brief Write the project layer and return its directory.
    std::filesystem::path project(const std::string& body) const {
        auto dir = root / "project";
        write_file(dir / "config.local.yaml", body);
        return dir;
    }
};

/// @brief A models block naming a tier that needs no GGUF on disk.
///
/// Present in most cases purely to keep `load_layered` off the
/// empty-tiers bundled-default fallback, which is exercised on purpose
/// by its own case below.
const char* models_block() {
    return "models:\n"
           "  default: lead\n"
           "  lead:\n"
           "    path: /nonexistent/entropic-gh163.gguf\n";
}

/// @brief Run the layered loader over a FakeHome, returning the merged config.
entropic::ParsedConfig layered(const FakeHome& home,
                               const std::filesystem::path& project_dir) {
    entropic::config::BundledModels registry;
    REQUIRE(registry.load(std::filesystem::path(TEST_DATA_DIR)
                          / "bundled_models.yaml").empty());
    entropic::ParsedConfig config;
    auto err = entropic::config::load_layered(
        project_dir, std::filesystem::path{}, registry, config);
    INFO("load_layered: " << err);
    REQUIRE(err.empty());
    return config;
}

/**
 * @brief Capture what the "prompts" logger emits while `fn` runs.
 * @param fn Work to run with the capture sink installed.
 * @return Everything the logger wrote.
 */
template <typename Fn>
std::string capture_prompt_log(Fn&& fn) {
    auto logger = entropic::log::get("prompts");
    auto saved = logger->sinks();
    std::ostringstream captured;
    logger->sinks() = {
        std::make_shared<spdlog::sinks::ostream_sink_mt>(captured)};
    auto restore = logger->level();
    logger->set_level(spdlog::level::trace);
    fn();
    logger->flush();
    logger->sinks() = saved;
    logger->set_level(restore);
    return captured.str();
}

}  // namespace

// ── Round-trip through the layers ───────────────────────────────────

SCENARIO("gh#163 a top-level app_context path survives the config layers",
         "[config][gh163][cpu]") {
    GIVEN("only a global config naming an absolute app_context path") {
        FakeHome home("global-only");
        home.global(std::string(models_block())
                    + "app_context: \"/abs/ctx/CLAUDE.md\"\n");
        auto config = layered(home, std::filesystem::path{});

        THEN("the path reaches ParsedConfig") {
            REQUIRE(config.app_context.has_value());
            CHECK(config.app_context->string() == "/abs/ctx/CLAUDE.md");
            CHECK_FALSE(config.app_context_disabled);
            CHECK_FALSE(config.app_context_content.has_value());
        }
    }

    GIVEN("a global app_context and a project layer that never mentions it") {
        FakeHome home("project-silent");
        home.global("app_context: \"/abs/ctx/CLAUDE.md\"\n");
        auto dir = home.project(models_block());
        auto config = layered(home, dir);

        THEN("the project layer does not erase what it did not set") {
            REQUIRE(config.app_context.has_value());
            CHECK(config.app_context->string() == "/abs/ctx/CLAUDE.md");
        }
    }

    GIVEN("a global app_context and a project layer with no models at all") {
        // The v2.11.1 empty-tiers fallback re-parses the bundled default
        // into a SCRATCH config precisely so it cannot overwrite settings an
        // explicit layer established. Pin that it still cannot.
        FakeHome home("bundled-fallback");
        home.global("app_context: \"/abs/ctx/CLAUDE.md\"\n");
        auto dir = home.project("log_level: DEBUG\n");
        auto config = layered(home, dir);

        THEN("the bundled-default transplant leaves app_context alone") {
            REQUIRE(config.app_context.has_value());
            CHECK(config.app_context->string() == "/abs/ctx/CLAUDE.md");
        }
    }

    GIVEN("both layers naming a path") {
        FakeHome home("project-wins");
        home.global("app_context: \"/abs/global.md\"\n");
        auto dir = home.project(std::string(models_block())
                                + "app_context: \"/abs/project.md\"\n");
        auto config = layered(home, dir);

        THEN("the more specific layer wins") {
            REQUIRE(config.app_context.has_value());
            CHECK(config.app_context->string() == "/abs/project.md");
        }
    }

    GIVEN("a global path the project layer turns off") {
        FakeHome home("project-false");
        home.global("app_context: \"/abs/global.md\"\n");
        auto dir = home.project(std::string(models_block())
                                + "app_context: false\n");
        auto config = layered(home, dir);

        THEN("false wins and no path survives") {
            CHECK(config.app_context_disabled);
            CHECK_FALSE(config.app_context.has_value());
        }
    }

    GIVEN("a global path and project-supplied inline content") {
        FakeHome home("project-inline");
        home.global("app_context: \"/abs/global.md\"\n");
        auto dir = home.project(std::string(models_block())
                                + "app_context:\n  content: |\n    Inline.\n");
        auto config = layered(home, dir);

        THEN("the content is carried") {
            REQUIRE(config.app_context_content.has_value());
            CHECK(config.app_context_content->find("Inline")
                  != std::string::npos);
        }
        THEN("and inline wins at load time, whatever the stale path says") {
            // extract_inline_content matches first and never clears the
            // path the earlier layer set, so the two can coexist on the
            // struct. load_app_context's ordering is what settles it.
            std::string body;
            auto err = entropic::prompts::load_app_context(
                config.app_context, config.app_context_content,
                config.app_context_disabled,
                std::filesystem::path("/nonexistent"), body);
            CHECK(err.empty());
            CHECK(body.find("Inline") != std::string::npos);
        }
    }

    GIVEN("a configured app_context and the env override pass") {
        // There is no ENTROPIC_APP_CONTEXT override; this pins that the
        // env pass cannot clear what a file layer set.
        FakeHome home("env-pass");
        home.global(std::string(models_block())
                    + "app_context: \"/abs/ctx/CLAUDE.md\"\n");
        auto config = layered(home, std::filesystem::path{});

        THEN("app_context is untouched by apply_env_overrides") {
            REQUIRE(config.app_context.has_value());
            CHECK(config.app_context->string() == "/abs/ctx/CLAUDE.md");
        }
    }
}

// ── The diagnostic: three states, three messages ────────────────────

SCENARIO("gh#163 the three app_context no-body states are distinguishable",
         "[config][gh163][cpu]") {
    GIVEN("nothing configured, and an explicit opt-out") {
        std::string body;
        auto absent = capture_prompt_log([&] {
            entropic::prompts::load_app_context(
                std::nullopt, std::nullopt, /*disabled=*/false,
                std::filesystem::path("/nonexistent"), body);
        });
        auto disabled = capture_prompt_log([&] {
            entropic::prompts::load_app_context(
                std::nullopt, std::nullopt, /*disabled=*/true,
                std::filesystem::path("/nonexistent"), body);
        });

        THEN("each state logs something, and the two do not read alike") {
            INFO("absent:   " << absent);
            INFO("disabled: " << disabled);
            CHECK_FALSE(absent.empty());
            CHECK_FALSE(disabled.empty());
            // Before the fix BOTH said "App context disabled (not
            // configured)" — one line covering two different situations,
            // one of which it actively misdescribed.
            CHECK(absent != disabled);
            CHECK(absent.find("not configured") != std::string::npos);
            CHECK(disabled.find("false") != std::string::npos);
        }
    }

    GIVEN("a configured path that cannot be parsed") {
        auto path = std::filesystem::temp_directory_path()
            / "entropic-gh163-rejected" / "CLAUDE.md";
        write_file(path, "# No frontmatter here.\n");
        std::string body = "sentinel";
        std::string log;
        auto err = std::string{};
        log = capture_prompt_log([&] {
            err = entropic::prompts::load_app_context(
                path, std::nullopt, /*disabled=*/false,
                std::filesystem::path("/nonexistent"), body);
        });

        THEN("the error is returned AND the log says it was rejected") {
            INFO("log: " << log);
            CHECK_FALSE(err.empty());
            CHECK(body.empty());
            CHECK(log.find("rejected") != std::string::npos);
            CHECK(log.find(path.string()) != std::string::npos);
            // The distinguishing property: this state must not be
            // reported with either of the other two states' wording.
            CHECK(log.find("not configured") == std::string::npos);
        }
    }
}
SCENARIO("gh#163 classify_app_context names the state the config is in",
         "[config][gh163][cpu]") {
    using entropic::prompts::AppContextState;
    using entropic::prompts::classify_app_context;

    GIVEN("each spelling") {
        THEN("the classification matches the config that produced it") {
            CHECK(classify_app_context(std::nullopt, std::nullopt, false)
                  == AppContextState::NOT_CONFIGURED);
            CHECK(classify_app_context(std::nullopt, std::nullopt, true)
                  == AppContextState::DISABLED);
            CHECK(classify_app_context(std::nullopt,
                                       std::optional<std::string>("x"), false)
                  == AppContextState::INLINE);
            CHECK(classify_app_context(std::filesystem::path("/a.md"),
                                       std::nullopt, false)
                  == AppContextState::FROM_PATH);
        }
        THEN("an explicit opt-out still beats inline content") {
            CHECK(classify_app_context(std::filesystem::path("/a.md"),
                                       std::optional<std::string>("x"), true)
                  == AppContextState::DISABLED);
        }
    }

    GIVEN("the message table") {
        std::vector<std::string> messages;
        for (auto s : {AppContextState::NOT_CONFIGURED,
                       AppContextState::DISABLED,
                       AppContextState::INLINE,
                       AppContextState::FROM_PATH}) {
            messages.emplace_back(
                entropic::prompts::app_context_state_message(s));
        }

        THEN("no two states share a message") {
            for (size_t i = 0; i < messages.size(); ++i) {
                CHECK_FALSE(messages[i].empty());
                for (size_t j = i + 1; j < messages.size(); ++j) {
                    INFO(messages[i] << " vs " << messages[j]);
                    CHECK(messages[i] != messages[j]);
                }
            }
        }
    }
}
