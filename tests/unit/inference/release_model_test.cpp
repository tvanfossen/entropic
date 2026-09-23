// SPDX-License-Identifier: Apache-2.0
/**
 * @file release_model_test.cpp
 * @brief gh#164 (v2.13.0) — explicit eviction that keeps everything else.
 *
 * A loaded model stayed in VRAM for the life of the handle: the engine
 * evicted only when a DIFFERENT tier needed the space, so a host wanting
 * the GPU back had to destroy the handle and lose every session's
 * conversation with it.
 *
 * These cases pin the orchestrator half of the contract — EVICTED fires,
 * the model is gone, the next use brings it back — against the small CPU
 * model the other unit-scope smokes use. The C ABI half (argument
 * validation, ALREADY_RUNNING) lives in tests/unit/api/entropic_capi_test.cpp
 * where no model is needed, and the adapter-registration half lives in
 * adapter_manager_test.cpp where the llama adapter API is mocked.
 *
 * SEPARATE binary: this is the one test that deliberately loads the same
 * model TWICE in one process (that is the feature), so it does not share a
 * process with any other real-model smoke. It runs CPU-only
 * (`gpu_layers = 0`), so the repeated load never touches a CUDA context.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/inference/orchestrator.h>
#include <entropic/types/config.h>
#include <entropic/types/error.h>

#include <cstdlib>
#include <filesystem>
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
    void clear() { events.clear(); tiers.clear(); }
};

void observe(ModelOrchestrator& orch, EventLog& log) {
    orch.set_residency_observer(
        [&log](ModelOrchestrator::ResidencyEvent event,
               const std::string& tier, const std::string&, size_t) {
            log.events.push_back(event);
            log.tiers.push_back(tier);
        });
}

std::filesystem::path small_model() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) { return {}; }
    return std::filesystem::path(home) /
           ".entropic" / "models" / "Qwen3.5-0.8B-Q8_0.gguf";
}

entropic::ParsedConfig one_tier(const std::filesystem::path& path) {
    entropic::ParsedConfig config;
    config.models.default_tier = "lead";
    entropic::TierConfig lead;
    lead.path = path;
    lead.adapter = "qwen35";
    lead.gpu_layers = 0;        // CPU-only — no CUDA context to re-enter
    lead.context_length = 512;
    lead.n_threads = 2;
    lead.use_mlock = false;
    lead.flash_attn = false;
    config.models.tiers["lead"] = lead;
    return config;
}

}  // namespace

SCENARIO("gh#164: releasing an unknown tier is refused, not silently ignored",
         "[residency][orchestrator][gh164][2.13.0]") {
    GIVEN("an orchestrator that was never initialized") {
        ModelOrchestrator orch;

        THEN("an unknown tier name is MODEL_NOT_FOUND") {
            CHECK(orch.release_models("no-such-tier")
                  == ENTROPIC_ERROR_MODEL_NOT_FOUND);
        }

        THEN("releasing everything is a no-op, not an error") {
            CHECK(orch.release_models("") == ENTROPIC_OK);
        }
    }
}

SCENARIO("gh#164: release evicts the model and the next use brings it back",
         "[residency][orchestrator][gh164][realmodel][2.13.0]") {
    auto path = small_model();
    if (path.empty() || !std::filesystem::exists(path)) {
        SUCCEED("Qwen3.5-0.8B-Q8_0.gguf absent — skipping release/reload "
                "residency smoke");
        return;
    }

    GIVEN("an eagerly-loaded default tier") {
        auto config = one_tier(path);
        ModelOrchestrator orch;
        REQUIRE(orch.initialize(config));
        REQUIRE(orch.loaded_models().size() == 1);

        EventLog log;
        observe(orch, log);   // after init: only release traffic is counted

        WHEN("the tier is released by name") {
            auto rc = orch.release_models("lead");

            THEN("it is evicted, announced, and no longer resident") {
                CHECK(rc == ENTROPIC_OK);
                CHECK(log.count(ModelOrchestrator::ResidencyEvent::Evicted)
                      == 1);
                REQUIRE_FALSE(log.tiers.empty());
                CHECK(log.tiers.front() == "lead");
                CHECK(orch.loaded_models().empty());
                // The active-tier record must be cleared too, or the next
                // activation thinks the incumbent is still there.
                CHECK(orch.last_used_tier().empty());
            }

            AND_THEN("the residency snapshot no longer lists it") {
                CHECK(orch.residency_snapshot_json()
                          .find("\"tier\":\"lead\"") == std::string::npos);
            }

            AND_WHEN("a caller needs the tier again") {
                log.clear();
                auto* model = orch.ensure_model("lead");

                THEN("it reloads lazily and says so") {
                    REQUIRE(model != nullptr);
                    CHECK(log.count(
                              ModelOrchestrator::ResidencyEvent::Loaded) == 1);
                    CHECK(orch.loaded_models().size() == 1);
                }
            }

            AND_WHEN("the same tier is released twice") {
                log.clear();
                auto again = orch.release_models("lead");

                THEN("the second release is a no-op, not an error") {
                    CHECK(again == ENTROPIC_OK);
                    CHECK(log.events.empty());
                }
            }
        }

        WHEN("everything is released with an empty tier name") {
            log.clear();
            auto rc = orch.release_models("");

            THEN("the tier is evicted just the same") {
                CHECK(rc == ENTROPIC_OK);
                CHECK(log.count(ModelOrchestrator::ResidencyEvent::Evicted)
                      == 1);
                CHECK(orch.loaded_models().empty());
            }
        }

        orch.shutdown();
    }
}
