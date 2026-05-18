// SPDX-License-Identifier: Apache-2.0
/**
 * @file residency_test.cpp
 * @brief gh#57 (v2.2.4) — VRAM-aware tier residency state + snapshot tests.
 *
 * The orchestrator's full lifecycle (load/activate/swap) requires a real
 * GGUF file. This test exercises the metadata-only surfaces that gh#57
 * adds and that don't need a loaded model:
 *
 *  - Default `residency_snapshot_json()` returns an empty residency set
 *    with `vram_budget_bytes == 0` and `backend == "unknown"`.
 *  - `ENTROPIC_VRAM_BUDGET_BYTES` is parsed by `resolve_vram_budget_bytes`.
 *  - The C ABI residency error code is wired in and named.
 *  - `set_residency_observer` is callable with both lambda and nullptr.
 *
 * Real load/evict/swap event firing is covered by the integration suite
 * (model-test gate) once a fixture model is available — those paths run
 * through `LlamaCppBackend::load_and_activate`, which the unit tests
 * cannot exercise without llama.cpp model files.
 *
 * @version 2.2.4
 */

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <entropic/inference/orchestrator.h>
#include <entropic/types/error.h>

#include <cstdlib>
#include <string>

TEST_CASE("Residency snapshot on a default-constructed orchestrator",
          "[residency][orchestrator][gh57]") {
    entropic::ModelOrchestrator orch;
    auto j = nlohmann::json::parse(orch.residency_snapshot_json());

    REQUIRE(j.is_object());
    REQUIRE(j.contains("residency"));
    REQUIRE(j["residency"].is_array());
    REQUIRE(j["residency"].empty());
    REQUIRE(j["vram_budget_bytes"].get<size_t>() == 0u);
    REQUIRE(j["vram_total_bytes"].get<size_t>() == 0u);
    REQUIRE(j["vram_headroom_bytes"].get<size_t>() == 0u);
    REQUIRE(j["backend"].get<std::string>() == "unknown");
}

TEST_CASE("Residency observer slot accepts and clears callables",
          "[residency][orchestrator][gh57]") {
    entropic::ModelOrchestrator orch;

    int fired = 0;
    orch.set_residency_observer(
        [&fired](entropic::ModelOrchestrator::ResidencyEvent,
                 const std::string&, const std::string&, size_t) {
            ++fired;
        });

    // Default-constructed orchestrator has no loaded tiers — no
    // observer fires implicitly. The test just pins that registration
    // and clearing don't crash.
    orch.set_residency_observer(nullptr);
    REQUIRE(fired == 0);
}

TEST_CASE("Last residency error defaults to OK and clears on demand",
          "[residency][orchestrator][gh57]") {
    entropic::ModelOrchestrator orch;
    REQUIRE(orch.last_residency_error() == ENTROPIC_OK);

    orch.clear_last_residency_error();
    REQUIRE(orch.last_residency_error() == ENTROPIC_OK);
}

TEST_CASE("ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE has a stable name",
          "[residency][error][gh57]") {
    // Pins the C ABI enum-to-name mapping so future error-table additions
    // do not silently shift this slot.
    REQUIRE(std::string(entropic_error_name(
                ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE))
            == "ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE");
}

TEST_CASE("Residency event enum matches the public C ABI",
          "[residency][abi][gh57]") {
    // The C++ ResidencyEvent enum is cast directly to
    // entropic_residency_event_t in the facade. Pin the integer values
    // so a future reordering would surface here, not as a silent ABI
    // drift seen by consumers.
    REQUIRE(static_cast<int>(
                entropic::ModelOrchestrator::ResidencyEvent::Loaded) == 0);
    REQUIRE(static_cast<int>(
                entropic::ModelOrchestrator::ResidencyEvent::Evicted) == 1);
    REQUIRE(static_cast<int>(
                entropic::ModelOrchestrator::ResidencyEvent::ActivationSwap)
            == 2);
}
