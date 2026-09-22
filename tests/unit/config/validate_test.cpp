// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_config_validate.cpp
 * @brief BDD tests for config validation functions.
 * @version 1.8.1
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/config/validate.h>
#include <entropic/prompts/manager.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace entropic;
using namespace entropic::config;

SCENARIO("ModelConfig validation", "[config][validate]") {
    GIVEN("A valid ModelConfig") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.context_length = 16384;

        WHEN("validate is called") {
            THEN("it passes") {
                REQUIRE(validate(cfg).empty());
            }
        }
    }

    GIVEN("ModelConfig with context_length below minimum") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.context_length = 256;

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails with range error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }

    GIVEN("ModelConfig with context_length above maximum") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.context_length = 200000;

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails with range error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }

    GIVEN("ModelConfig with empty adapter") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.adapter = "";

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("adapter") != std::string::npos);
            }
        }
    }
}

SCENARIO("Allowed tools validation", "[config][validate]") {
    GIVEN("Tools with proper server.tool format") {
        std::vector<std::string> tools = {"filesystem.read_file",
                                          "git.status"};

        WHEN("validate_allowed_tools is called") {
            THEN("it passes") {
                REQUIRE(validate_allowed_tools(tools).empty());
            }
        }
    }

    GIVEN("Tools without dot separator") {
        std::vector<std::string> tools = {"bare_name"};

        WHEN("validate_allowed_tools is called") {
            auto err = validate_allowed_tools(tools);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("bare_name") != std::string::npos);
            }
        }
    }
}

SCENARIO("ModelsConfig validation", "[config][validate]") {
    GIVEN("Default tier not in tiers") {
        ModelsConfig cfg;
        TierConfig tier;
        tier.path = "/tmp/model.gguf";
        cfg.tiers["lead"] = tier;
        cfg.default_tier = "nonexistent";

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails with clear error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("nonexistent") != std::string::npos);
                REQUIRE(err.find("not in tiers") != std::string::npos);
            }
        }
    }

    GIVEN("Default tier exists in tiers") {
        ModelsConfig cfg;
        TierConfig tier;
        tier.path = "/tmp/model.gguf";
        cfg.tiers["lead"] = tier;
        cfg.default_tier = "lead";

        WHEN("validate is called") {
            THEN("it passes") {
                REQUIRE(validate(cfg).empty());
            }
        }
    }
}

SCENARIO("RoutingConfig validation", "[config][validate]") {
    ModelsConfig models;
    TierConfig tier;
    tier.path = "/tmp/model.gguf";
    models.tiers["lead"] = tier;
    models.tiers["eng"] = tier;
    models.default_tier = "lead";

    GIVEN("routing enabled without router model") {
        RoutingConfig routing;
        routing.enabled = true;
        routing.fallback_tier = "lead";

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("router is not configured")
                        != std::string::npos);
            }
        }
    }

    GIVEN("fallback tier not in tiers") {
        RoutingConfig routing;
        routing.fallback_tier = "nonexistent";

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("nonexistent") != std::string::npos);
            }
        }
    }

    GIVEN("tier_map references undefined tier") {
        RoutingConfig routing;
        routing.fallback_tier = "lead";
        routing.tier_map["code"] = "nonexistent";

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("nonexistent") != std::string::npos);
            }
        }
    }

    GIVEN("routing enabled + classification_prompt set but tier_map empty") {
        // v2.8.1 (review #4): an empty tier_map passes validate_tier_map (it
        // only checks map VALUES), so a configured classification_prompt with
        // no tier_map silently re-creates the v2.8.0 no-op. Router must be
        // present so validation reaches the new cross-field check.
        RoutingConfig routing;
        routing.enabled = true;
        routing.fallback_tier = "lead";
        routing.classification_prompt = "Classify: 1=eng 2=qa\n";
        // tier_map intentionally left empty
        models.router = ModelConfig{};
        models.router->path = "/tmp/router.gguf";

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails — the prompt is inert without a tier_map") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("tier_map is empty") != std::string::npos);
            }
        }
    }

    GIVEN("handoff_rules source not in tiers") {
        RoutingConfig routing;
        routing.fallback_tier = "lead";
        routing.handoff_rules["missing"] = {"lead"};

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("missing") != std::string::npos);
            }
        }
    }

    GIVEN("handoff_rules target not in tiers") {
        RoutingConfig routing;
        routing.fallback_tier = "lead";
        routing.handoff_rules["lead"] = {"missing"};

        WHEN("validate_routing is called") {
            auto err = validate_routing(routing, models);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("missing") != std::string::npos);
            }
        }
    }
}

SCENARIO("CompactionConfig validation", "[config][validate]") {
    GIVEN("Warning threshold >= compaction threshold") {
        CompactionConfig cfg;
        cfg.threshold_percent = 0.75f;
        cfg.warning_threshold_percent = 0.8f;

        WHEN("validate is called") {
            auto err = validate(cfg);

            THEN("it fails") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("warning_threshold_percent")
                        != std::string::npos);
            }
        }
    }

    GIVEN("Valid compaction config") {
        CompactionConfig cfg;
        cfg.threshold_percent = 0.75f;
        cfg.warning_threshold_percent = 0.6f;

        WHEN("validate is called") {
            THEN("it passes") {
                REQUIRE(validate(cfg).empty());
            }
        }
    }

    // ── v2.3.10: cover remaining failure branches ──

    GIVEN("threshold_percent below the 0.5 floor") {
        CompactionConfig cfg;
        cfg.threshold_percent = 0.3f;
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a threshold_percent error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("threshold_percent") != std::string::npos);
            }
        }
    }

    GIVEN("preserve_recent_turns above the cap") {
        CompactionConfig cfg;
        cfg.threshold_percent = 0.75f;
        cfg.warning_threshold_percent = 0.6f;
        cfg.preserve_recent_turns = 99;
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a preserve_recent_turns error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("preserve_recent_turns")
                        != std::string::npos);
            }
        }
    }

    GIVEN("tool_result_ttl below 1") {
        CompactionConfig cfg;
        cfg.threshold_percent = 0.75f;
        cfg.warning_threshold_percent = 0.6f;
        cfg.preserve_recent_turns = 3;
        cfg.tool_result_ttl = 0;
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a tool_result_ttl error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("tool_result_ttl") != std::string::npos);
            }
        }
    }
}

// ── v2.3.10: cover remaining ModelConfig + ModelsConfig branches ──

SCENARIO("ModelConfig validation — failure modes",
         "[config][validate][v2.3.10][failure-mode]") {
    GIVEN("context_length below the 512 floor") {
        ModelConfig cfg;
        cfg.context_length = 256;
        cfg.adapter = "qwen35";
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a context_length error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }

    GIVEN("context_length above the ceiling") {
        ModelConfig cfg;
        cfg.context_length = 999999;
        cfg.adapter = "qwen35";
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a context_length error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("context_length") != std::string::npos);
            }
        }
    }

    GIVEN("empty adapter") {
        ModelConfig cfg;
        cfg.adapter.clear();
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with an adapter-empty error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("adapter") != std::string::npos);
            }
        }
    }

    GIVEN("n_batch below 1") {
        ModelConfig cfg;
        cfg.adapter = "qwen35";
        cfg.n_batch = 0;
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with an n_batch error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("n_batch") != std::string::npos);
            }
        }
    }
}

SCENARIO("ModelsConfig validation — default tier must exist",
         "[config][validate][v2.3.10]") {
    GIVEN("a ModelsConfig whose default points at a missing tier") {
        ModelsConfig cfg;
        cfg.default_tier = "missing";
        TierConfig tier;
        tier.adapter = "qwen35";
        cfg.tiers["lead"] = tier;
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it fails with a 'default tier' error") {
                REQUIRE_FALSE(err.empty());
                REQUIRE(err.find("default tier") != std::string::npos);
            }
        }
    }

    GIVEN("an empty tiers map") {
        ModelsConfig cfg;
        cfg.default_tier = "anything";
        WHEN("validate is called") {
            auto err = validate(cfg);
            THEN("it passes (no tiers means no default-tier check)") {
                REQUIRE(err.empty());
            }
        }
    }
}

// ── gh#154 (v2.13.0): an unresolved tier grammar stem is a WARNING here ──
//
// `GrammarRegistry::get()` returns "" on a miss and the decode proceeds
// UNCONSTRAINED — documented fail-open. From outside it is undetectable:
// constrained and unconstrained output have the same SHAPE whenever the
// prompt also describes the shape, so the only signal was the ABSENCE of a
// `Registered grammar '<key>'` line. A consumer measured speculative decode
// for three days believing a grammar was active; their harness wrote each
// arm's config where no matching `.gbnf` sat. Two accept-rate figures and
// one throughput figure were withdrawn.
//
// gh#154 first made this a configure-time ERROR. That was wrong, and the
// GPU gate proved it: `entropic_grammar_register` and
// `entropic_grammar_register_file` both require an orchestrator, which
// exists only AFTER `entropic_configure*`, so refusing at configure made
// "configure, then register this tier's grammar" — the only sequence open
// to a consumer holding its grammar in memory — impossible to perform.
// Configure therefore WARNS, naming every unresolved tier; the refusal
// lands at first use as ENTROPIC_ERROR_GRAMMAR_NOT_FOUND
// (tests/unit/inference/tier_grammar_gate_test.cpp).
//
// A runtime `params.grammar_key` is NOT checked at either point — it may
// name a grammar registered later — and its miss is reported through
// `generations[].grammar.resolved` instead.

namespace {

/// @brief A temp directory holding `<stem>.gbnf` for each stem given.
std::filesystem::path gh154_grammar_dir(
    const std::string& tag, const std::vector<std::string>& stems) {
    auto dir = std::filesystem::temp_directory_path()
        / ("entropic-gh154-" + tag) / "grammars";
    std::filesystem::remove_all(dir.parent_path());
    std::filesystem::create_directories(dir);
    for (const auto& stem : stems) {
        std::ofstream out(dir / (stem + ".gbnf"));
        out << "root ::= \"x\"\n";
    }
    return dir;
}

/// @brief A config with one tier naming `grammar`.
ParsedConfig gh154_config(const std::string& grammar) {
    ParsedConfig config;
    config.models.default_tier = "lead";
    TierConfig lead;
    lead.adapter = "qwen35";
    if (!grammar.empty()) {
        lead.grammar = std::filesystem::path(grammar);
    }
    config.models.tiers["lead"] = lead;
    return config;
}

}  // namespace

SCENARIO("gh#154 an unresolved tier grammar stem is warned about, not "
         "refused",
         "[config][validate][gh154][cpu]") {
    GIVEN("a tier naming a stem that is present") {
        auto dir = gh154_grammar_dir("present", {"compactor"});
        auto warning =
            warn_unresolved_tier_grammars(gh154_config("compactor"), {dir});

        THEN("there is nothing to say") {
            INFO(warning);
            CHECK(warning.empty());
        }
    }

    GIVEN("a tier naming the same grammar WITH the .gbnf extension") {
        // normalize_grammar_key strips it, so both spellings name one
        // grammar and the check has to agree with the registry.
        auto dir = gh154_grammar_dir("extension", {"compactor"});
        auto warning = warn_unresolved_tier_grammars(
            gh154_config("compactor.gbnf"), {dir});

        THEN("still nothing to say") {
            INFO(warning);
            CHECK(warning.empty());
        }
    }

    GIVEN("a tier naming a stem that is absent") {
        auto dir = gh154_grammar_dir("absent", {"something-else"});
        auto warning =
            warn_unresolved_tier_grammars(gh154_config("compactor"), {dir});

        THEN("it warns, naming the tier, the stem and where it looked") {
            REQUIRE_FALSE(warning.empty());
            CHECK(warning.find("lead") != std::string::npos);
            CHECK(warning.find("compactor") != std::string::npos);
            // "not found" without saying WHERE it looked is the
            // diagnostic that cost three days.
            CHECK(warning.find(dir.string()) != std::string::npos);
        }
        AND_THEN("it names the call that resolves it — the whole reason "
                 "this is a warning and not a refusal") {
            CHECK(warning.find("entropic_grammar_register")
                  != std::string::npos);
        }
    }

    GIVEN("TWO tiers naming absent stems") {
        // A warning is advisory, so it has to be complete: stopping at
        // the first would hide the second until a later run.
        auto dir = gh154_grammar_dir("absent-two", {});
        auto config = gh154_config("compactor");
        TierConfig second;
        second.adapter = "qwen35";
        second.grammar = std::filesystem::path("scribe");
        config.models.tiers["editor"] = second;
        auto warning = warn_unresolved_tier_grammars(config, {dir});

        THEN("BOTH are named in one message") {
            REQUIRE_FALSE(warning.empty());
            CHECK(warning.find("compactor") != std::string::npos);
            CHECK(warning.find("scribe") != std::string::npos);
            CHECK(warning.find("editor") != std::string::npos);
        }
    }

    GIVEN("a tier with no grammar at all") {
        auto warning = warn_unresolved_tier_grammars(gh154_config(""), {});

        THEN("nothing to say — the check is opt-in with the key") {
            CHECK(warning.empty());
        }
    }

    GIVEN("a tier naming a stem and NO search path configured") {
        auto warning =
            warn_unresolved_tier_grammars(gh154_config("compactor"), {});

        THEN("it warns and says no search path exists") {
            REQUIRE_FALSE(warning.empty());
            CHECK(warning.find("search path") != std::string::npos);
        }
    }

    GIVEN("two search paths, the stem in the second") {
        auto first = gh154_grammar_dir("two-a", {});
        auto second = gh154_grammar_dir("two-b", {"compactor"});
        auto warning = warn_unresolved_tier_grammars(
            gh154_config("compactor"), {first, second});

        THEN("either path resolving is enough") {
            INFO(warning);
            CHECK(warning.empty());
        }
    }
}

SCENARIO("gh#154 grammar search paths mirror the runtime lookup",
         "[config][validate][gh154][cpu]") {
    GIVEN("a config_dir whose grammars/ holds a .gbnf") {
        auto dir = gh154_grammar_dir("paths-populated", {"compactor"});
        auto config = gh154_config("compactor");
        config.config_dir = dir.parent_path();
        auto paths = grammar_search_paths(config, "/data");

        THEN("only config_dir is searched — the facade fallback never runs") {
            // load_grammars_from(data_dir/grammars) is called ONLY when
            // the config_dir load registered nothing. Listing data_dir
            // anyway would accept a stem the registry will not hold.
            REQUIRE(paths.size() == 1);
            CHECK(paths.front() == dir);
        }
    }

    GIVEN("a config_dir whose grammars/ holds nothing") {
        auto dir = gh154_grammar_dir("paths-empty", {});
        auto config = gh154_config("compactor");
        config.config_dir = dir.parent_path();
        auto paths = grammar_search_paths(config, "/data");

        THEN("the data_dir fallback is searched too, exactly as at runtime") {
            REQUIRE(paths.size() == 2);
            CHECK(paths.front() == dir);
            CHECK(paths.back() == std::filesystem::path("/data/grammars"));
        }
    }

    GIVEN("no config_dir") {
        auto paths = grammar_search_paths(gh154_config("compactor"), "/data");

        THEN("the bundled data directory is the only search path") {
            REQUIRE(paths.size() == 1);
            CHECK(paths.front() == std::filesystem::path("/data/grammars"));
        }
    }
}

// ── gh#154: entropic's OWN bundled identities must name real grammars ──
//
// Found while gating unresolvable tier stems: three shipped identities
// (`scribe`, `compactor`, `benchmark_judge`) declared
// `grammar: grammars/<name>.gbnf` and `data/grammars/` has only ever
// contained `constitutional_critique.gbnf` — git history shows the other
// three files never existed. Those keys had been inert since they were
// written, and gh#95 (v2.7.4) "fixed" the threading of exactly this field so
// it would reach `resolve_grammar_key`, where it silently missed.
//
// So the fail-open gh#154 reports was not only reachable by consumers: the
// engine's own default config had been running three tiers unconstrained
// while declaring otherwise. This walks the shipped data directory so the
// next dead key fails here instead of in a consumer's measurements.

SCENARIO("gh#154 every bundled identity grammar stem resolves",
         "[config][validate][gh154][cpu]") {
    // TEST_DATA_DIR arrives unnormalized ("<repo>/tests/unit/../data"), so
    // walking up from it verbatim lands inside tests/ instead of the repo.
    const auto data_dir =
        std::filesystem::weakly_canonical(std::filesystem::path(TEST_DATA_DIR))
            .parent_path().parent_path() / "data";
    const auto prompts = data_dir / "prompts";

    GIVEN("the shipped identity prompts") {
        REQUIRE(std::filesystem::is_directory(prompts));

        std::vector<std::string> unresolved;
        for (const auto& entry : std::filesystem::directory_iterator(prompts)) {
            if (entry.path().extension() != ".md") { continue; }
            entropic::prompts::ParsedIdentity identity;
            if (!entropic::prompts::load_identity(
                    entry.path(), identity).empty()) {
                continue;  // not an identity file; parse_prompt_file owns that
            }
            if (!identity.frontmatter.grammar.has_value()) { continue; }
            const auto stem =
                std::filesystem::path(*identity.frontmatter.grammar)
                    .stem().string();
            if (!std::filesystem::exists(
                    data_dir / "grammars" / (stem + ".gbnf"))) {
                unresolved.push_back(
                    entry.path().filename().string() + " → " + stem);
            }
        }

        THEN("none of them names a grammar that does not ship") {
            INFO("unresolved: " << unresolved.size());
            for (const auto& u : unresolved) { UNSCOPED_INFO(u); }
            CHECK(unresolved.empty());
        }
    }
}

SCENARIO("gh#153 #42(iii): cpu_moe_layers is refused at configure time when "
         "it cannot mean what it says",
         "[config][validate][expert_offload][gh153][2.13.0]") {
    GIVEN("a tier that never mentions cpu_moe_layers") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";

        THEN("validation is unaffected — the omitted key is the default and "
             "the default is off") {
            REQUIRE(cfg.cpu_moe_layers == 0);
            CHECK(validate(cfg).empty());
        }
    }

    GIVEN("a tier with an explicit split and an explicit expert offload") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.gpu_layers = 31;
        cfg.cpu_moe_layers = 18;

        THEN("it validates") {
            CHECK(validate(cfg).empty());
        }
    }

    GIVEN("a negative cpu_moe_layers") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.gpu_layers = 31;
        cfg.cpu_moe_layers = -1;

        WHEN("validate is called") {
            const auto err = validate(cfg);

            THEN("it is refused rather than read as 'all layers'") {
                REQUIRE_FALSE(err.empty());
                INFO(err);
                CHECK(err.find("cpu_moe_layers") != std::string::npos);
            }
        }
    }

    GIVEN("cpu_moe_layers on a CPU-only tier") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.gpu_layers = 0;
        cfg.cpu_moe_layers = 8;

        WHEN("validate is called") {
            const auto err = validate(cfg);

            THEN("it is refused instead of silently doing nothing") {
                REQUIRE_FALSE(err.empty());
                INFO(err);
                CHECK(err.find("gpu_layers: 0") != std::string::npos);
            }
        }
    }

    GIVEN("cpu_moe_layers alongside gpu_layers: auto") {
        ModelConfig cfg;
        cfg.path = "/tmp/model.gguf";
        cfg.gpu_layers_auto = true;
        cfg.cpu_moe_layers = 8;

        WHEN("validate is called") {
            const auto err = validate(cfg);

            THEN("it is refused — the admission gate prices whole layers and "
                 "knows nothing about expert placement") {
                REQUIRE_FALSE(err.empty());
                INFO(err);
                CHECK(err.find("gpu_layers: auto") != std::string::npos);
            }
        }
    }
}
