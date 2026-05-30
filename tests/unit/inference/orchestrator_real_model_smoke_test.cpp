// SPDX-License-Identifier: Apache-2.0
/**
 * @file orchestrator_real_model_smoke_test.cpp
 * @brief gh#87 (v2.7.0): unit-scope real-model smoke for the
 *        ModelOrchestrator generate path. Drives initialize →
 *        generate (no-tools + tools) → generate_streaming → route →
 *        shutdown, covering resolve_and_stage / stage_active_tools /
 *        apply_adapter_parse + the per-tier generate dispatch that the
 *        mock-config orchestrator_test cannot reach.
 *
 * Loads $HOME/.entropic/models/Qwen3.5-0.8B-Q8_0.gguf (~0.8GB, CPU-only,
 * gpu_layers=0). WARN-skips when absent so the suite still runs.
 *
 * SEPARATE binary (own process): exactly one model load per process, so
 * it never hits the repeated-load CUDA-shutdown SEGV documented in
 * backend_real_model_smoke_test.cpp (which loads in entropic-inference-
 * tests). Keeping these two real-model loads in different processes is
 * the deliberate isolation.
 *
 * @version 2.7.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/inference/orchestrator.h>
#include <entropic/types/config.h>
#include <entropic/types/message.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::filesystem::path small_model() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) { return {}; }
    return std::filesystem::path(home) /
           ".entropic" / "models" / "Qwen3.5-0.8B-Q8_0.gguf";
}
}  // namespace

TEST_CASE("ModelOrchestrator real-model generate (one load, CPU)",
          "[orchestrator][realmodel][gh87][topup]") {
    auto path = small_model();
    if (path.empty() || !std::filesystem::exists(path)) {
        SUCCEED("Qwen3.5-0.8B-Q8_0.gguf absent — skipping real-model "
                "orchestrator smoke");
        return;
    }

    entropic::ParsedConfig config;
    config.models.default_tier = "lead";
    entropic::TierConfig lead;
    lead.path = path;
    lead.adapter = "qwen35";
    lead.gpu_layers = 0;        // CPU-only (coverage build has no CUDA)
    lead.context_length = 512;  // small — coverage, not throughput
    lead.n_threads = 2;
    lead.use_mlock = false;
    lead.flash_attn = false;
    lead.keep_warm = true;
    config.models.tiers["lead"] = lead;

    entropic::ModelOrchestrator orch;
    REQUIRE(orch.initialize(config));  // create_tier_backends + activate

    std::vector<entropic::Message> msgs;
    entropic::Message u;
    u.role = "user";
    u.content = "Say hi.";
    msgs.push_back(u);

    entropic::GenerationParams params;
    params.max_tokens = 4;
    params.temperature = 0.0f;
    params.enable_thinking = false;

    // No-tools generate — per-tier dispatch + apply_adapter_parse.
    auto r1 = orch.generate(msgs, params, "lead");
    CHECK(r1.error_message.empty());

    // Tools staged — resolve_and_stage → stage_active_tools →
    // set_active_tools on the backend; apply_adapter_parse on the result.
    params.tools =
        R"([{"name":"read_file","description":"Read a file",)"
        R"("inputSchema":{"type":"object","properties":)"
        R"({"path":{"type":"string"}},"required":["path"]}}])";
    auto r2 = orch.generate(msgs, params, "lead");
    CHECK(r2.error_message.empty());

    // Streaming generate + cancel reference path.
    std::atomic<bool> cancel{false};
    auto r3 = orch.generate_streaming(
        msgs, params, [](std::string_view) {}, cancel, "lead");
    (void)r3;

    // Routing + last-used-tier accessors.
    (void)orch.route(msgs);
    (void)orch.last_used_tier();

    orch.shutdown();
}
