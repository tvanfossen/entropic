// SPDX-License-Identifier: Apache-2.0
/**
 * @file deferred_load_test.cpp
 * @brief gh#157 (v2.13.0) — `models.defer_load` keeps the default tier out
 *        of VRAM until something asks for it.
 *
 * Two consumers opening one editor session loaded the same 4.8 GB GGUF
 * twice and held ~9.6 GB of an 11 GB card without ever being sent a run,
 * because `ModelOrchestrator::initialize` called `activate_default_tier`
 * unconditionally. `keep_warm` documented "pre-warm at startup" and
 * controlled nothing of the sort.
 *
 * The cheap half of this needs no GGUF: whether init TOUCHES the model file
 * at all is observable from a tier pointed at a file that exists and is not
 * a model. Eager init fails on it; deferred init does not look.
 *
 * The event-count half needs a real load, so it uses the same
 * $HOME/.entropic/models/Qwen3.5-0.8B-Q8_0.gguf the other unit-scope
 * smokes use and WARN-skips when it is absent.
 *
 * SEPARATE binary: exactly one model load per process, the same isolation
 * orchestrator_real_model_smoke_test.cpp documents.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/inference/orchestrator.h>
#include <entropic/types/config.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using entropic::ModelOrchestrator;

/// @brief Residency events captured in registration order.
struct EventLog {
    std::vector<ModelOrchestrator::ResidencyEvent> events;
    std::vector<std::string> tiers;

    size_t count(ModelOrchestrator::ResidencyEvent want) const {
        size_t n = 0;
        for (auto e : events) {
            if (e == want) { ++n; }
        }
        return n;
    }
};

/// @brief Attach a capturing observer to an orchestrator.
void observe(ModelOrchestrator& orch, EventLog& log) {
    orch.set_residency_observer(
        [&log](ModelOrchestrator::ResidencyEvent event,
               const std::string& tier, const std::string&, size_t) {
            log.events.push_back(event);
            log.tiers.push_back(tier);
        });
}

/// @brief A file that exists and is definitively not a GGUF.
std::filesystem::path write_not_a_model() {
    auto path = std::filesystem::temp_directory_path()
        / "entropic-gh157-not-a-model.bin";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "this is not a GGUF file";
    return path;
}

/// @brief The small CPU model the unit-scope smokes share.
std::filesystem::path small_model() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) { return {}; }
    return std::filesystem::path(home) /
           ".entropic" / "models" / "Qwen3.5-0.8B-Q8_0.gguf";
}

/// @brief One-tier config pointed at `path`.
entropic::ParsedConfig one_tier(const std::filesystem::path& path,
                                bool defer_load) {
    entropic::ParsedConfig config;
    config.models.default_tier = "lead";
    config.models.defer_load = defer_load;
    entropic::TierConfig lead;
    lead.path = path;
    lead.adapter = "qwen35";
    lead.gpu_layers = 0;        // CPU-only — the coverage build has no CUDA
    lead.context_length = 512;
    lead.n_threads = 2;
    lead.use_mlock = false;
    lead.flash_attn = false;
    config.models.tiers["lead"] = lead;
    return config;
}

}  // namespace

SCENARIO("gh#157: defer_load decides whether init touches the model at all",
         "[residency][orchestrator][gh157][2.13.0]") {
    GIVEN("a default tier pointed at a file that is not a loadable model") {
        const auto path = write_not_a_model();

        WHEN("defer_load is false — the pre-2.13.0 behaviour") {
            auto config = one_tier(path, false);
            ModelOrchestrator orch;
            EventLog log;
            observe(orch, log);

            THEN("init reaches the model file, and fails on it") {
                // This is the load that two idle sumac hosts each paid for.
                REQUIRE_FALSE(orch.initialize(config));
            }
        }

        WHEN("defer_load is true") {
            auto config = one_tier(path, true);
            ModelOrchestrator orch;
            EventLog log;
            observe(orch, log);

            THEN("init succeeds without ever opening the model") {
                REQUIRE(orch.initialize(config));
                CHECK(orch.loaded_models().empty());
                CHECK(log.events.empty());
                CHECK(orch.last_used_tier().empty());
            }
        }
    }
}

SCENARIO("gh#157: a deferred tier answers vision from config while unloaded",
         "[residency][orchestrator][gh157][2.13.0]") {
    GIVEN("a vision-declaring tier that has not been loaded") {
        auto config = one_tier(write_not_a_model(), true);
        config.models.tiers["lead"].capabilities = {"text", "vision"};
        entropic::TierConfig plain = config.models.tiers["lead"];
        plain.capabilities = {"text"};
        config.models.tiers["plain"] = plain;

        ModelOrchestrator orch;
        REQUIRE(orch.initialize(config));
        REQUIRE(orch.loaded_models().empty());

        THEN("the capability is readable without a backend") {
            // Pre-2.13.0 the facade asked the BACKEND, whose has_vision_ is
            // set during activation — so a deferred vision tier reported 0,
            // which is wrong rather than unknown.
            CHECK(orch.tier_declares_vision("lead"));
            CHECK_FALSE(orch.tier_declares_vision("plain"));
            CHECK_FALSE(orch.tier_declares_vision("nonexistent"));
        }
    }
}

SCENARIO("gh#157: the first use loads exactly once, through the gate",
         "[residency][orchestrator][gh157][realmodel][2.13.0]") {
    auto path = small_model();
    if (path.empty() || !std::filesystem::exists(path)) {
        SUCCEED("Qwen3.5-0.8B-Q8_0.gguf absent — skipping deferred-load "
                "residency-event smoke");
        return;
    }

    GIVEN("a deferred default tier on a real GGUF") {
        auto config = one_tier(path, true);
        ModelOrchestrator orch;
        EventLog log;
        observe(orch, log);

        REQUIRE(orch.initialize(config));

        THEN("initialize fires no residency event and loads nothing") {
            CHECK(log.count(ModelOrchestrator::ResidencyEvent::Loaded) == 0);
            CHECK(orch.loaded_models().empty());
        }

        WHEN("the first caller asks for the model") {
            auto* model = orch.ensure_model("lead");

            THEN("it is loaded, once, with a Loaded event naming the tier") {
                REQUIRE(model != nullptr);
                CHECK(log.count(ModelOrchestrator::ResidencyEvent::Loaded)
                      == 1);
                REQUIRE_FALSE(log.tiers.empty());
                CHECK(log.tiers.front() == "lead");
                CHECK(orch.loaded_models().size() == 1);
                CHECK(orch.last_used_tier() == "lead");
            }

            AND_WHEN("a second caller asks for the same tier") {
                auto* again = orch.ensure_model("lead");

                THEN("the resident model is reused, not reloaded") {
                    CHECK(again == model);
                    CHECK(log.count(
                              ModelOrchestrator::ResidencyEvent::Loaded) == 1);
                }
            }
        }

        orch.shutdown();
    }
}
