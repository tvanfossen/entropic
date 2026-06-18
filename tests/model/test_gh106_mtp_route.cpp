// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh106_mtp_route.cpp
 * @brief gh#106/gh#108: the COMBINED call path for MTP through the orchestrator.
 *
 * test_gh106_mtp / test_gh108_mtp_guards exercise LlamaCppBackend::generate_mtp
 * in isolation. Per the "test the combined call path" rule, this drives the FULL
 * production route a consumer hits via `inference.speculative.{enabled,mtp}`:
 *
 *   orchestrator->generate()
 *     → run_generate_dispatch (speculative.enabled gate)
 *       → try_speculative_route → try_speculative_route_streaming
 *         → (speculative.mtp) try_mtp_route → LlamaCppBackend::generate_mtp
 *
 * Two combined-path invariants:
 *  - gh#106: a greedy in-envelope call actually engages MTP (n_drafted>0) — a
 *    wiring bug would silently drop to plain decode (n_drafted==0).
 *  - gh#108: an out-of-envelope/misconfigured call PROPAGATES the loud typed
 *    error (ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG) — try_mtp_route must
 *    NOT silently fall back to plain decode (which would mask the bad config).
 *
 * Builds its own orchestrator (no global model listener) so the dev box config
 * is untouched.
 */

#include "model_test_context.h"  // helpers only — NO CATCH_REGISTER_LISTENER

#include <atomic>
#include <cstdio>

namespace {

std::filesystem::path models_dir() {
    return std::filesystem::path(getenv("HOME")) / ".entropic" / "models";
}

// Repoint the default tier at the gemma4 target + enable MTP (head as
// draft.path). Leaves ctx.config populated but does NOT init the orchestrator,
// so a caller can tweak (e.g. n_draft) before init. SKIPs when models/tier are
// absent. Returns the default tier name.
std::string configure_mtp_route(ModelTestContext& ctx) {
    auto target = models_dir() / "gemma-4-E2B-it-Q8_0.gguf";
    auto head = models_dir() / "mtp-gemma-4-E2B-it.gguf";
    if (!std::filesystem::is_regular_file(target) ||
        !std::filesystem::is_regular_file(head)) {
        SKIP("MTP target/head GGUF not present");
    }
    REQUIRE(load_registry(ctx.registry));
    REQUIRE(load_test_config(ctx.registry, ctx.config));
    auto tier_name = ctx.config.models.default_tier;
    auto it = ctx.config.models.tiers.find(tier_name);
    if (it == ctx.config.models.tiers.end()) {
        SKIP("no default tier in config to repoint");
    }
    auto& tier = it->second;
    tier.path = target;
    tier.adapter = "gemma4";
    tier.gpu_layers = 99;
    tier.context_length = 4096;
    tier.grammar.reset();  // greedy, unconstrained — the MTP envelope
    auto& spec = ctx.config.inference.speculative;
    spec.enabled = true;
    spec.mtp = true;
    spec.draft.path = head;
    spec.n_draft = 16;
    return tier_name;
}

entropic::GenerationParams greedy_params() {
    entropic::GenerationParams p;
    p.max_tokens = 48;
    p.temperature = 0.0f;
    return p;
}

}  // namespace

TEST_CASE("gh#106 MTP routes through orchestrator->generate()",
          "[model][gh106][mtp][route]") {
    ModelTestContext ctx;
    auto tier_name = configure_mtp_route(ctx);
    if (!init_orchestrator(ctx)) {
        SKIP("orchestrator init failed (resource/config) — route untested");
    }

    entropic::Message u;
    u.role = "user";
    u.content = "Continue exactly, one number per line, up to 20:\n1\n2\n3\n4\n";
    auto r = ctx.orchestrator->generate({u}, greedy_params(), tier_name);

    std::printf("\n===gh106 MTP route===\ntier=%s drafted=%d accepted=%d "
                "finish=%s\n[%s]\n===\n", tier_name.c_str(), r.n_drafted,
                r.n_accepted, r.finish_reason.c_str(), r.content.c_str());
    INFO("drafted=" << r.n_drafted << " accepted=" << r.n_accepted);

    REQUIRE(r.error_code == 0);
    REQUIRE_FALSE(r.content.empty());
    // Decisive combined-path gate: the orchestrator actually dispatched MTP
    // (n_drafted is only non-zero when generate_mtp ran).
    REQUIRE(r.n_drafted > 0);
    CHECK(r.n_accepted > 0);
}

TEST_CASE("gh#108 MTP loud error propagates through orchestrator (no fallback)",
          "[model][gh108][mtp][route]") {
    ModelTestContext ctx;
    auto tier_name = configure_mtp_route(ctx);
    // Oversized draft window — a config-driven trigger independent of tier
    // temp/grammar resolution. The kernel must reject it LOUDLY.
    ctx.config.inference.speculative.n_draft = 100000;
    if (!init_orchestrator(ctx)) {
        SKIP("orchestrator init failed (resource/config) — route untested");
    }

    entropic::Message u;
    u.role = "user";
    u.content = "Count to five.";
    auto r = ctx.orchestrator->generate({u}, greedy_params(), tier_name);

    std::printf("\n===gh108 MTP route fail-loud===\ncode=%d msg=%s drafted=%d\n===\n",
                r.error_code, r.error_message.c_str(), r.n_drafted);

    // The loud error reached the caller — NOT a silent plain-decode fallback.
    // A fallback would give error_code==0 + non-empty content + n_drafted==0.
    REQUIRE(r.error_code == ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG);
    REQUIRE(r.error_message.find("n_batch") != std::string::npos);
    REQUIRE(r.n_drafted == 0);
}
