// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_config_loader.cpp
 * @brief BDD tests for config loading and layered merge.
 * @version 1.8.1
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <entropic/config/loader.h>
#include <entropic/config/bundled_models.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

/**
 * @brief Return the path to the test data directory.
 * @return Filesystem path defined by TEST_DATA_DIR compile definition.
 * @version 1.8.1
 * @internal
 */
static std::filesystem::path test_data()
{
    return std::filesystem::path(TEST_DATA_DIR);
}

/**
 * @brief Load the test bundled models registry from test data.
 * @return Populated BundledModels registry.
 * @version 1.8.1
 * @internal
 */
static entropic::config::BundledModels load_test_registry()
{
    entropic::config::BundledModels registry;
    auto err = registry.load(test_data() / "bundled_models.yaml");
    REQUIRE(err.empty());
    return registry;
}

SCENARIO("Parse minimal config", "[config][loader]") {
    GIVEN("YAML with only log_level") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;

        WHEN("parse_config_file is called") {
            auto err = entropic::config::parse_config_file(
                test_data() / "minimal_config.yaml", registry, config);

            THEN("it succeeds") {
                REQUIRE(err.empty());
            }

            THEN("log_level is overridden") {
                REQUIRE(config.log_level == "DEBUG");
            }

            THEN("all other fields have defaults") {
                REQUIRE(config.models.default_tier == "lead");
                REQUIRE(config.routing.enabled == false);
                REQUIRE(config.compaction.threshold_percent
                        == Catch::Approx(0.75f));
            }
        }
    }
}

SCENARIO("Parse config with tiers", "[config][loader]") {
    GIVEN("Config with lead and eng tiers") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;

        WHEN("parse_config_file is called") {
            auto err = entropic::config::parse_config_file(
                test_data() / "test_config.yaml", registry, config);

            THEN("it succeeds") {
                REQUIRE(err.empty());
            }

            THEN("tiers are parsed") {
                REQUIRE(config.models.tiers.size() == 2);
                REQUIRE(config.models.tiers.count("lead") == 1);
                REQUIRE(config.models.tiers.count("eng") == 1);
            }

            THEN("tier fields are correct") {
                auto& lead = config.models.tiers["lead"];
                REQUIRE(lead.adapter == "qwen35");
                REQUIRE(lead.context_length == 131072);
                REQUIRE(lead.gpu_layers == 40);
            }

            THEN("model paths are resolved from registry") {
                // v2.0.5: resolve() falls back to ~/.entropic/models/<name>.gguf
                // when ENTROPIC_MODEL_DIR is unset and no candidate exists on
                // disk. The previous hardcoded ~/models/gguf/ path is gone.
                auto& lead = config.models.tiers["lead"];
                auto home = std::filesystem::path(std::getenv("HOME"));
                auto expected = home / ".entropic" / "models"
                                / "Qwen3.5-35B-A3B-UD-IQ3_XXS.gguf";
                REQUIRE(lead.path == expected);
            }

            THEN("MCP config is parsed") {
                REQUIRE(config.mcp.enable_filesystem == true);
                REQUIRE(config.mcp.filesystem.allow_outside_root == true);
            }
        }
    }
}

SCENARIO("Layered merge — project overlays global", "[config][loader]") {
    GIVEN("Global config with lead.gpu_layers=40") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;

        // Load global config first
        auto err = entropic::config::parse_config_file(
            test_data() / "test_config.yaml", registry, config);
        REQUIRE(err.empty());

        WHEN("project config overrides only gpu_layers") {
            err = entropic::config::parse_config_file(
                test_data() / "overlay_config.yaml", registry, config);

            THEN("it succeeds") {
                REQUIRE(err.empty());
            }

            THEN("gpu_layers is overridden") {
                REQUIRE(config.models.tiers["lead"].gpu_layers == 20);
            }

            THEN("adapter is preserved from global") {
                REQUIRE(config.models.tiers["lead"].adapter == "qwen35");
            }

            THEN("eng tier is preserved from global") {
                REQUIRE(config.models.tiers.count("eng") == 1);
            }
        }
    }
}

// ── v2.0.6: Consumer models block replace semantics ──────

SCENARIO("Consumer models block replaces global tiers",
         "[config][loader][v2.0.6]")
{
    GIVEN("a global config with eng tier and a consumer with lead+researcher") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;
        // Load global first (has eng)
        auto err = entropic::config::parse_config_file(
            test_data() / "test_config.yaml", registry, config);
        REQUIRE(err.empty());
        REQUIRE(config.models.tiers.count("eng") == 1);

        WHEN("consumer config with models: block is overlaid with replace") {
            // Simulate replace semantics: clear tiers before consumer parse
            config.models.tiers.clear();
            config.models.router.reset();
            config.models.default_tier.clear();
            err = entropic::config::parse_config_file(
                test_data() / "consumer_replace_config.yaml",
                registry, config);

            THEN("only consumer tiers survive") {
                REQUIRE(err.empty());
                CHECK(config.models.tiers.count("lead") == 1);
                CHECK(config.models.tiers.count("researcher") == 1);
                CHECK(config.models.tiers.count("eng") == 0);
                CHECK(config.models.tiers.size() == 2);
            }

            THEN("default tier is set from consumer") {
                CHECK(config.models.default_tier == "lead");
            }
        }
    }
}

// ── v2.3.10: comprehensive parse coverage ─────────────────────

SCENARIO("Comprehensive config exercises every parse_* helper",
         "[config][loader][v2.3.10][coverage]")
{
    GIVEN("a YAML config touching every loader section") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;

        WHEN("parse_config_file is called on the comprehensive fixture") {
            auto err = entropic::config::parse_config_file(
                test_data() / "comprehensive_config.yaml", registry, config);
            REQUIRE(err.empty());

            THEN("top-level scalar fields parse") {
                CHECK(config.log_level == "WARN");
                CHECK(config.inject_model_context == true);
                CHECK(config.vram_reserve_mb == 512);
                CHECK(config.ggml_logging == true);
                CHECK(config.console_logging == false);
                REQUIRE(config.constitution.has_value());
                REQUIRE(config.app_context.has_value());
            }

            THEN("models section parses default + router + tiers") {
                CHECK(config.models.default_tier == "lead");
                REQUIRE(config.models.router.has_value());
                CHECK(config.models.router->adapter == "qwen35");
                CHECK(config.models.router->context_length == 8192);
                REQUIRE(config.models.tiers.count("lead") == 1);
                auto& lead = config.models.tiers["lead"];
                CHECK(lead.adapter == "qwen35");
                CHECK(lead.context_length == 131072);
                CHECK(lead.gpu_layers == 40);
                CHECK(lead.keep_warm == true);
                CHECK(lead.use_mlock == true);
                CHECK(lead.reasoning_budget == 2000);
                CHECK(lead.cache_type_k == "q8_0");
                CHECK(lead.cache_type_v == "q8_0");
                CHECK(lead.n_batch == 1024);
                CHECK(lead.n_ubatch == 256);  // gh#23 v2.3.17
                CHECK(lead.n_threads == 8);
                CHECK(lead.tensor_split == "0.5,0.5");
                CHECK(lead.split_mode == "row");  // gh#23 v2.3.18
                CHECK(lead.main_gpu == 1);        // gh#23 v2.3.19
                CHECK(lead.offload_kqv == false); // gh#23 v2.3.20
                CHECK(lead.rope_freq_base == 100000.0f); // gh#23 v2.3.21
                CHECK(lead.rope_freq_scale == 0.5f); // gh#23 v2.3.22
                CHECK(lead.n_parallel == 4); // gh#23 v2.3.23
                CHECK(lead.flash_attn == true);
                REQUIRE(lead.allowed_tools.has_value());
                CHECK(lead.allowed_tools->size() == 2);
                CHECK_FALSE(lead.mmproj_path.empty());
            }

            THEN("routing section parses enabled + maps") {
                CHECK(config.routing.enabled == true);
                CHECK(config.routing.fallback_tier == "lead");
                REQUIRE(config.routing.classification_prompt.has_value());
                CHECK(config.routing.classification_prompt->find(
                          "Pick the right tier") != std::string::npos);
                CHECK(config.routing.tier_map.size() == 2);
                CHECK(config.routing.tier_map.at("code") == "eng");
                CHECK(config.routing.handoff_rules.size() == 2);
            }

            THEN("compaction section parses every field") {
                CHECK(config.compaction.enabled == true);
                CHECK(config.compaction.threshold_percent
                      == Catch::Approx(0.8f));
                CHECK(config.compaction.preserve_recent_turns == 5);
                CHECK(config.compaction.summary_max_tokens == 512);
                CHECK(config.compaction.notify_user == true);
                CHECK(config.compaction.save_full_history == true);
                CHECK(config.compaction.tool_result_ttl == 10);
                CHECK(config.compaction.warning_threshold_percent
                      == Catch::Approx(0.6f));
            }

            THEN("permissions section parses allow/deny/auto_approve") {
                CHECK(config.permissions.allow.size() == 2);
                CHECK(config.permissions.deny.size() == 1);
                CHECK(config.permissions.auto_approve == true);
            }

            THEN("mcp section parses enables + nested filesystem + external") {
                CHECK(config.mcp.enable_entropic == true);
                CHECK(config.mcp.enable_filesystem == true);
                CHECK(config.mcp.enable_bash == false);
                CHECK(config.mcp.enable_git == true);
                CHECK(config.mcp.enable_diagnostics == true);
                CHECK(config.mcp.enable_web == true);
                CHECK(config.mcp.server_timeout_seconds == 30);
                CHECK_FALSE(config.mcp.working_dir.empty());
                CHECK(config.mcp.filesystem.diagnostics_on_edit == true);
                CHECK(config.mcp.filesystem.fail_on_errors == false);
                CHECK(config.mcp.filesystem.diagnostics_timeout == 10);
                CHECK(config.mcp.filesystem.allow_outside_root == true);
                CHECK(config.mcp.filesystem.max_read_context_pct
                      == Catch::Approx(0.5f));
                CHECK(config.mcp.filesystem.max_read_bytes == 65536);
                CHECK(config.mcp.external.enabled == true);
                CHECK(config.mcp.external.rate_limit == 100);
                REQUIRE(config.mcp.external.socket_path.has_value());
            }

            THEN("generation section parses defaults") {
                CHECK(config.generation.max_tokens == 2048);
                CHECK(config.generation.default_temperature
                      == Catch::Approx(0.5f));
                CHECK(config.generation.default_top_p
                      == Catch::Approx(0.85f));
            }

            THEN("lsp section parses enables") {
                CHECK(config.lsp.enabled == true);
                CHECK(config.lsp.python_enabled == true);
                CHECK(config.lsp.c_enabled == false);
            }

            THEN("inference.prompt_cache and inference.speculative parse") {
                CHECK(config.prompt_cache.enabled == false);
                CHECK(config.prompt_cache.log_hits == false);
                CHECK(config.prompt_cache.max_bytes == 268435456);
                CHECK(config.inference.speculative.enabled == true);
                CHECK(config.inference.speculative.n_draft == 8);
                CHECK(config.inference.speculative.draft.context_length
                      == 4096);
                CHECK(config.inference.speculative.draft.flash_attn == false);
            }

            THEN("constitutional_validation parses every field") {
                CHECK(config.constitutional_validation.enabled == true);
                CHECK(config.constitutional_validation.max_revisions == 2);
                CHECK(config.constitutional_validation.max_critique_tokens
                      == 256);
                CHECK(config.constitutional_validation.temperature
                      == Catch::Approx(0.2f));
                CHECK(config.constitutional_validation.enable_thinking
                      == false);
                CHECK(config.constitutional_validation.priority == 50);
                CHECK(config.constitutional_validation.grammar_key
                      == "constitutional");
                CHECK(config.constitutional_validation.skip_tiers.size() == 1);
            }
        }
    }
}

SCENARIO("load_config_from_string drives parse_config_string + validate",
         "[config][loader][v2.3.10][coverage]")
{
    GIVEN("a minimal in-memory YAML string") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;
        std::string yaml =
            "models:\n"
            "  lead:\n"
            "    path: primary\n"
            "    adapter: qwen35\n"
            "    context_length: 8192\n"
            "    gpu_layers: 0\n"
            "  default: lead\n";

        WHEN("load_config_from_string is called") {
            auto err = entropic::config::load_config_from_string(
                yaml, registry, config);

            THEN("it succeeds and parses the inline tier") {
                REQUIRE(err.empty());
                CHECK(config.models.default_tier == "lead");
                CHECK(config.models.tiers.count("lead") == 1);
            }
        }
    }

    GIVEN("an empty string") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;

        WHEN("load_config_from_string is called with empty content") {
            auto err = entropic::config::load_config_from_string(
                "", registry, config);

            THEN("it returns a descriptive error (no segfault)") {
                CHECK_FALSE(err.empty());
                CHECK(err.find("empty") != std::string::npos);
            }
        }
    }

    GIVEN("a non-map root (a scalar string)") {
        auto registry = load_test_registry();
        entropic::ParsedConfig config;

        WHEN("load_config_from_string is called with a scalar root") {
            auto err = entropic::config::load_config_from_string(
                "just_a_scalar\n", registry, config);

            THEN("parse_config_string rejects with a 'not a mapping' error") {
                CHECK_FALSE(err.empty());
                CHECK(err.find("mapping") != std::string::npos);
            }
        }
    }
}

SCENARIO("parse_config_file rejects missing file with read error",
         "[config][loader][v2.3.10][coverage][failure-mode]")
{
    auto registry = load_test_registry();
    entropic::ParsedConfig config;

    WHEN("parse_config_file is called on a non-existent path") {
        auto err = entropic::config::parse_config_file(
            test_data() / "this-file-does-not-exist.yaml",
            registry, config);

        THEN("it returns a 'cannot read' error") {
            CHECK_FALSE(err.empty());
            CHECK(err.find("cannot read") != std::string::npos);
        }
    }
}

SCENARIO("load_config_from_file forwards file errors through the chain",
         "[config][loader][v2.3.10][coverage][failure-mode]")
{
    auto registry = load_test_registry();
    entropic::ParsedConfig config;

    WHEN("load_config_from_file is called on a non-existent path") {
        auto err = entropic::config::load_config_from_file(
            test_data() / "missing.yaml", registry, config);

        THEN("it returns the parse-layer error") {
            CHECK_FALSE(err.empty());
        }
    }

    WHEN("load_config_from_file is called on the comprehensive fixture") {
        auto err = entropic::config::load_config_from_file(
            test_data() / "comprehensive_config.yaml", registry, config);

        THEN("it parses and validates cleanly") {
            REQUIRE(err.empty());
            CHECK(config.models.default_tier == "lead");
        }
    }
}

SCENARIO("load_layered overlays a project config.local.yaml when present",
         "[config][loader][v2.3.10][coverage][layered]")
{
    auto registry = load_test_registry();

    GIVEN("a project dir whose config.local.yaml seeds models + log_level") {
        auto project_dir = std::filesystem::temp_directory_path()
            / "v2.3.10-layered-project";
        std::filesystem::remove_all(project_dir);
        std::filesystem::create_directories(project_dir);

        // config.local.yaml lives at the project_dir root (not under
        // .entropic/) per `load_project_layer`'s lookup at line 986.
        auto project_path = project_dir / "config.local.yaml";
        std::ofstream out(project_path);
        out << "log_level: DEBUG\n"
            << "models:\n"
            << "  lead:\n"
            << "    path: primary\n"
            << "    adapter: qwen35\n"
            << "  default: lead\n";
        out.close();

        WHEN("load_layered is called pointing at the project dir") {
            entropic::ParsedConfig config;
            auto err = entropic::config::load_layered(
                project_dir,
                std::filesystem::path{},  // no consumer defaults
                registry, config);

            THEN("the project layer's overrides are applied") {
                REQUIRE(err.empty());
                CHECK(config.log_level == "DEBUG");
                CHECK(config.models.default_tier == "lead");
                // load_layered sets log_dir = project_dir when log_dir
                // was empty after layers were applied — exercising the
                // gate at loader.cpp:1042.
                CHECK(config.log_dir == project_dir);
            }
        }
    }
}

SCENARIO("load_config drives global + project + env + validate chain",
         "[config][loader][v2.3.10][coverage][load_config]")
{
    auto registry = load_test_registry();
    auto tmp = std::filesystem::temp_directory_path() / "v2.3.10-load_config";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto global_path = tmp / "global.yaml";
    {
        std::ofstream g(global_path);
        g << "log_level: INFO\n"
          << "models:\n"
          << "  lead:\n"
          << "    path: primary\n"
          << "    adapter: qwen35\n"
          << "  default: lead\n";
    }

    auto project_path = tmp / "project.yaml";
    {
        std::ofstream p(project_path);
        p << "log_level: DEBUG\n";
    }

    GIVEN("load_config is called with both layers present") {
        entropic::ParsedConfig config;
        auto err = entropic::config::load_config(
            global_path, project_path, registry, config);

        THEN("project overrides global, validation runs cleanly") {
            REQUIRE(err.empty());
            CHECK(config.log_level == "DEBUG");
            CHECK(config.models.default_tier == "lead");
        }
    }

    GIVEN("load_config is called with only global (project missing)") {
        entropic::ParsedConfig config;
        auto err = entropic::config::load_config(
            global_path, tmp / "missing.yaml", registry, config);

        THEN("global layer alone is used") {
            REQUIRE(err.empty());
            CHECK(config.log_level == "INFO");
        }
    }

    GIVEN("load_config with both missing falls into load_bundled_default") {
        entropic::ParsedConfig config;
        auto err = entropic::config::load_config(
            tmp / "missing-global.yaml",
            tmp / "missing-project.yaml",
            registry, config);

        THEN("falls through; result depends on bundled default availability") {
            // The bundled-default code path runs; whether it finds a
            // bundled default_config.yaml depends on the runtime
            // install state. Either error or empty err is acceptable.
            (void)err;
        }
    }
}

SCENARIO("load_config_from_file is the single-file public entry",
         "[config][loader][v2.3.10][coverage][load_config_from_file]")
{
    auto registry = load_test_registry();

    GIVEN("the comprehensive YAML on disk") {
        entropic::ParsedConfig config;
        auto err = entropic::config::load_config_from_file(
            test_data() / "comprehensive_config.yaml", registry, config);

        THEN("parse + env_overrides + validate run in sequence") {
            REQUIRE(err.empty());
            CHECK(config.log_level == "WARN");
        }
    }

    GIVEN("a non-existent path") {
        entropic::ParsedConfig config;
        auto err = entropic::config::load_config_from_file(
            test_data() / "definitely-not-here.yaml", registry, config);

        THEN("the parse error short-circuits validation") {
            CHECK_FALSE(err.empty());
        }
    }
}

SCENARIO("load_layered discovers .mcp.json from project dir",
         "[config][loader][v2.3.10][coverage][discover_mcp_json]")
{
    auto registry = load_test_registry();
    auto project_dir = std::filesystem::temp_directory_path()
        / "v2.3.10-discover-mcp";
    std::filesystem::remove_all(project_dir);
    std::filesystem::create_directories(project_dir);

    // Project layer config.local.yaml satisfies models so the
    // load_bundled_default fallback isn't reached.
    {
        std::ofstream p(project_dir / "config.local.yaml");
        p << "models:\n"
          << "  lead:\n"
          << "    path: primary\n"
          << "    adapter: qwen35\n"
          << "  default: lead\n";
    }

    GIVEN("a .mcp.json with stdio + sse entries in the project dir") {
        {
            std::ofstream m(project_dir / ".mcp.json");
            m << "{\n"
              << "  \"mcpServers\": {\n"
              << "    \"stdio-server\": {\n"
              << "      \"type\": \"stdio\",\n"
              << "      \"command\": \"/bin/echo\",\n"
              << "      \"args\": [\"hello\", \"world\"],\n"
              << "      \"env\": {\"FOO\": \"bar\"}\n"
              << "    },\n"
              << "    \"sse-server\": {\n"
              << "      \"type\": \"sse\",\n"
              << "      \"url\": \"https://example.com/mcp\"\n"
              << "    }\n"
              << "  }\n"
              << "}\n";
        }

        WHEN("load_layered runs with the project dir") {
            entropic::ParsedConfig config;
            auto err = entropic::config::load_layered(
                project_dir, std::filesystem::path{}, registry, config);

            THEN("both external MCP servers were discovered + parsed") {
                REQUIRE(err.empty());
                CHECK(config.mcp.external_servers.count("stdio-server") == 1);
                CHECK(config.mcp.external_servers.count("sse-server") == 1);
                auto& stdio = config.mcp.external_servers["stdio-server"];
                CHECK(stdio.command == "/bin/echo");
                CHECK(stdio.args.size() == 2);
                CHECK(stdio.env.at("FOO") == "bar");
                auto& sse = config.mcp.external_servers["sse-server"];
                CHECK(sse.url == "https://example.com/mcp");
            }
        }
    }

    GIVEN("a malformed .mcp.json") {
        {
            std::ofstream m(project_dir / ".mcp.json");
            m << "{ not valid json";
        }

        WHEN("load_layered runs") {
            entropic::ParsedConfig config;
            auto err = entropic::config::load_layered(
                project_dir, std::filesystem::path{}, registry, config);

            THEN("the malformed file is reported but doesn't fail the load") {
                REQUIRE(err.empty());
                CHECK(config.mcp.external_servers.empty());
            }
        }
    }
}

SCENARIO("load_layered's consumer-defaults layer overlays before project",
         "[config][loader][v2.3.10][coverage][layered]")
{
    auto registry = load_test_registry();

    GIVEN("a consumer-defaults YAML on disk and a project layer that overrides") {
        auto tmp = std::filesystem::temp_directory_path()
            / "v2.3.10-layered-consumer";
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp);

        auto consumer_path = tmp / "consumer_defaults.yaml";
        std::ofstream cd(consumer_path);
        cd << "log_level: WARN\n"
           << "models:\n"
           << "  lead:\n"
           << "    path: primary\n"
           << "    adapter: qwen35\n"
           << "    context_length: 4096\n"
           << "  default: lead\n";
        cd.close();

        auto project_path = tmp / "config.local.yaml";
        std::ofstream pp(project_path);
        // Project doesn't redefine models — only bumps log_level.
        pp << "log_level: ERROR\n";
        pp.close();

        WHEN("load_layered is called with both consumer and project paths") {
            entropic::ParsedConfig config;
            auto err = entropic::config::load_layered(
                tmp, consumer_path, registry, config);

            THEN("project overrides consumer (more specific wins)") {
                REQUIRE(err.empty());
                CHECK(config.log_level == "ERROR");
                CHECK(config.models.tiers.count("lead") == 1);
                CHECK(config.models.tiers["lead"].context_length == 4096);
            }
        }
    }
}
