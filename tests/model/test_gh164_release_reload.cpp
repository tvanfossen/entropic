// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh164_release_reload.cpp
 * @brief gh#164 (v2.13.0): release a resident model and generate again.
 *
 * The unit suite proves the bookkeeping — EVICTED fires, the registration
 * survives, the next `ensure_model` reloads. What it cannot prove is that
 * the reloaded model DECODES CORRECTLY, and that is where a release/reload
 * cycle can go wrong quietly: the reload rebuilds the llama context and the
 * KV memory, and this repository has shipped a KV-position desync once
 * already (gh#97, v2.7.6) by exercising a cache path on plain-KV gemma4 and
 * never on a hybrid architecture.
 *
 * So both families run here:
 *   - gemma4 E2B, plain llama_kv_cache.
 *   - Qwen3.6-35B-A3B (arch qwen35moe), attention + recurrent/SSM hybrid,
 *     at gpu_layers=15 — the partial-offload split the 11 GB dev card needs.
 *
 * Each case asserts the same deterministic desync gate test_gh97_hybrid_cache
 * uses: a correct prefill leaves `kv_pos_max` ≈ input + generated - 1, so it
 * must stay below `input + max_tokens`. An un-removed tail inflates it.
 *
 * The third case drives the PUBLIC C ABI (`entropic_release_model`) through
 * the facade harness, because the orchestrator-direct cases bypass
 * `configure_common` — the seam that hid gh#88/90/94.
 *
 * Requires: GPU + the two GGUFs. Run: ctest -L model -R gh164
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>
#include <entropic/inference/orchestrator.h>
#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include "../../src/inference/llama_cpp_backend.h"
#include "facade_model_helpers.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path gguf(const std::string& name) {
    const char* home = std::getenv("HOME");
    if (home == nullptr) { return {}; }
    return fs::path(home) / ".entropic" / "models" / name;
}

/// @brief One tier, one model, everything else default.
entropic::ParsedConfig tier_config(const fs::path& path,
                                   const std::string& adapter,
                                   int gpu_layers) {
    entropic::ParsedConfig config;
    config.models.default_tier = "lead";
    entropic::TierConfig lead;
    lead.path = path;
    lead.adapter = adapter;
    lead.context_length = 4096;
    lead.gpu_layers = gpu_layers;
    lead.flash_attn = false;
    // Partial offload keeps a large slice of the file on the CPU side;
    // mlock would pin it into unevictable RAM (v2.12.0 finding).
    lead.use_mlock = (gpu_layers < 0);
    config.models.tiers["lead"] = lead;
    return config;
}

/// @brief What one turn tells us about KV bookkeeping.
struct TurnFacts {
    entropic::GenerationResult result;
    int input = 0;
    int pos_max = 0;
};

TurnFacts run_turn(entropic::ModelOrchestrator& orch,
                   const std::vector<entropic::Message>& msgs,
                   const entropic::GenerationParams& params) {
    TurnFacts facts;
    facts.result = orch.generate(msgs, params, "lead");
    auto* llama = dynamic_cast<entropic::LlamaCppBackend*>(
        orch.get_backend("lead"));
    if (llama != nullptr) {
        facts.input = llama->last_input_tokens();
        facts.pos_max = llama->kv_pos_max();
    }
    return facts;
}

/// @brief Drive load → turn → release → turn on one model.
void release_reload_cycle(const fs::path& path, const std::string& adapter,
                          int gpu_layers) {
    auto config = tier_config(path, adapter, gpu_layers);
    entropic::ModelOrchestrator orch;

    int evicted = 0;
    int loaded = 0;
    orch.set_residency_observer(
        [&](entropic::ModelOrchestrator::ResidencyEvent e,
            const std::string&, const std::string&, size_t) {
            if (e == entropic::ModelOrchestrator::ResidencyEvent::Evicted) {
                ++evicted;
            } else if (e ==
                       entropic::ModelOrchestrator::ResidencyEvent::Loaded) {
                ++loaded;
            }
        });

    REQUIRE(orch.initialize(config));
    REQUIRE(orch.loaded_models().size() == 1);

    std::vector<entropic::Message> msgs;
    msgs.push_back({"system",
        "You are a terse assistant. Answer in one short sentence."});
    msgs.push_back({"user", "Name one primary colour."});

    entropic::GenerationParams params;
    params.max_tokens = 16;
    params.temperature = 0.0f;
    params.enable_thinking = false;

    const TurnFacts before = run_turn(orch, msgs, params);

    REQUIRE(orch.release_models("lead") == ENTROPIC_OK);
    CHECK(evicted == 1);
    CHECK(orch.loaded_models().empty());

    // The turn after a release is the whole point: it must reload and decode,
    // not fail on a context that is no longer there.
    msgs.push_back({"assistant", before.result.content.empty()
                                     ? std::string("(none)")
                                     : before.result.content});
    msgs.push_back({"user", "Name another one."});
    const TurnFacts after = run_turn(orch, msgs, params);

    CHECK(orch.loaded_models().size() == 1);

    INFO("before: input=" << before.input << " pos_max=" << before.pos_max
         << " finish=[" << before.result.finish_reason << "] content=["
         << before.result.content << "]");
    INFO("after:  input=" << after.input << " pos_max=" << after.pos_max
         << " finish=[" << after.result.finish_reason << "] content=["
         << after.result.content << "]");

    CHECK(before.result.error_code == 0);
    CHECK_FALSE(before.result.content.empty());
    CHECK(after.result.error_code == 0);
    CHECK_FALSE(after.result.content.empty());

    // Deterministic KV desync gate (gh#97): a correct prefill leaves
    // pos_max ≈ input + generated - 1. A reload that left stale memory
    // bookkeeping behind inflates it past input + max_tokens.
    CHECK(before.pos_max < before.input + params.max_tokens);
    CHECK(after.pos_max < after.input + params.max_tokens);

    orch.shutdown();
}

}  // namespace

TEST_CASE("gh#164: release then reload decodes correctly on gemma4 (plain KV)",
          "[model][gh164]") {
    auto path = gguf("gemma-4-E2B-it-Q8_0.gguf");
    if (!fs::is_regular_file(path)) {
        SKIP("gemma-4-E2B-it-Q8_0.gguf not present at " + path.string());
    }
    release_reload_cycle(path, "gemma4", 99);
}

TEST_CASE("gh#164: release then reload decodes correctly on a hybrid arch",
          "[model][gh164]") {
    // qwen35moe: attention + recurrent/SSM. Every KV-touching change is
    // exercised here as well as on plain KV — gh#97 shipped a desync
    // precisely because a cache path was only ever tested on gemma4.
    auto path = gguf("Qwen3.6-35B-A3B-UD-IQ3_XXS.gguf");
    if (!fs::is_regular_file(path)) {
        SKIP("Qwen3.6-35B-A3B GGUF not present at " + path.string());
    }
    release_reload_cycle(path, "qwen36", 15);  // 11 GB card: partial offload
}

TEST_CASE("gh#164: entropic_release_model frees VRAM through the C ABI",
          "[model][gh164]") {
    // Production path: configure_dir, not a hand-built orchestrator.
    namespace facade = entropic::test::facade;
    if (!fs::is_regular_file(facade::model_gguf("gemma-4-E2B-it-Q8_0.gguf"))) {
        SKIP("gemma-4-E2B-it-Q8_0.gguf not present");
    }

    facade::FacadeProject project("gh164_release");
    facade::TierSpec lead;
    lead.name = "lead";
    lead.gguf_key = "gemma4_e2b";
    lead.adapter = "gemma4";
    lead.context_length = 4096;
    // Guard and tier already name one model; the MENU did not fit. The
    // v2.13.0 gate log for this very case
    // (build/test-reports/model/logs/test-gh164-release-reload-c3.log)
    // reads "Active tools staged ...: 18594 bytes" / "Stream: 5014 input
    // tokens" against "n_ctx=4096", with "Decode chunk failed" on every
    // turn — and eleven assertions passed anyway. Since 26884f6 that is a
    // typed refusal (ENTROPIC_ERROR_EVAL_CONTEXT_FULL, terminal
    // `context_overflow`), so the REQUIREs below would fail instead. This
    // case is about VRAM eviction and lazy reload, not tool use, but the
    // tier contract derives `explicit_completion: true` — so it names
    // exactly the one tool it owes (1,595 bytes, ~400 tokens).
    lead.allowed_tools = {"entropic.complete"};
    auto* handle = project.setup({lead});
    INFO("setup: " << project.setup_failure());
    REQUIRE(handle != nullptr);

    static int evicted = 0;
    evicted = 0;
    REQUIRE(entropic_set_residency_observer(
        handle,
        [](entropic_residency_event_t event, const char*, const char*,
           size_t, void*) {
            if (event == ENTROPIC_RESIDENCY_EVICTED) { ++evicted; }
        },
        nullptr) == ENTROPIC_OK);

    char* first = nullptr;
    REQUIRE(entropic_run(handle, "Say hello in three words.", &first)
            == ENTROPIC_OK);
    entropic_free(first);

    REQUIRE(entropic_release_model(handle, nullptr) == ENTROPIC_OK);
    CHECK(evicted >= 1);

    char* snapshot = nullptr;
    REQUIRE(entropic_residency_snapshot(handle, &snapshot) == ENTROPIC_OK);
    std::string json(snapshot);
    entropic_free(snapshot);
    INFO("snapshot after release: " << json);
    CHECK(json.find("\"tier\":\"lead\"") == std::string::npos);

    // The conversation survives the release — that is the whole reason this
    // call exists rather than destroying and recreating the handle.
    char* history = nullptr;
    REQUIRE(entropic_context_get(handle, &history) == ENTROPIC_OK);
    std::string conversation(history);
    entropic_free(history);
    CHECK(conversation.find("Say hello in three words.") != std::string::npos);

    // And the next turn reloads lazily.
    char* second = nullptr;
    REQUIRE(entropic_run(handle, "And now say goodbye.", &second)
            == ENTROPIC_OK);
    REQUIRE(second != nullptr);
    entropic_free(second);
}
