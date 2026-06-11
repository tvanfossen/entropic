// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh106_mtp_route.cpp
 * @brief gh#106 (v2.9.0): the COMBINED call path for MTP.
 *
 * test_gh106_mtp exercises LlamaCppBackend::generate_mtp in isolation. Per the
 * "test the combined call path" rule, this drives the FULL production route a
 * consumer hits when they set `inference.speculative.{enabled,mtp}` in YAML:
 *
 *   orchestrator->generate()
 *     → run_generate_dispatch (speculative.enabled gate)
 *       → try_speculative_route → try_speculative_route_streaming
 *         → (speculative.mtp) try_mtp_route
 *           → LlamaCppBackend::generate_mtp
 *
 * A wiring bug — wrong gate, mis-forwarded head path / n_draft, or the
 * activate_draft MTP-skip loading the head as a phantom draft backend — would
 * silently drop MTP to plain decode. The result.n_drafted assertion fails RED
 * on exactly that. Deliberately does NOT register the global model listener:
 * it builds its own orchestrator so the dev box's default config is untouched.
 */

#include "model_test_context.h"  // helpers only — NO CATCH_REGISTER_LISTENER

#include <atomic>
#include <cstdio>

namespace {

std::filesystem::path models_dir() {
    return std::filesystem::path(getenv("HOME")) / ".entropic" / "models";
}

}  // namespace

TEST_CASE("gh#106 MTP routes through orchestrator->generate()",
          "[model][gh106][mtp][route]") {
    using namespace entropic;

    auto target = models_dir() / "gemma-4-E2B-it-Q8_0.gguf";
    auto head = models_dir() / "mtp-gemma-4-E2B-it.gguf";
    if (!std::filesystem::is_regular_file(target)) {
        SKIP("MTP target GGUF not present: " + target.string());
    }
    if (!std::filesystem::is_regular_file(head)) {
        SKIP("MTP head GGUF not present: " + head.string());
    }

    ModelTestContext ctx;
    REQUIRE(load_registry(ctx.registry));
    REQUIRE(load_test_config(ctx.registry, ctx.config));

    // Repoint the default tier at the gemma4 target + enable MTP on it. The
    // head GGUF flows in as speculative.draft.path; n_draft is the window.
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
    tier.grammar.reset();  // plain decode — no grammar constraint

    auto& spec = ctx.config.inference.speculative;
    spec.enabled = true;
    spec.mtp = true;
    spec.draft.path = head;
    spec.n_draft = 16;

    if (!init_orchestrator(ctx)) {
        SKIP("orchestrator init failed (resource/config) — route untested");
    }

    Message u;
    u.role = "user";
    u.content = "Continue exactly, one number per line, up to 20:\n1\n2\n3\n4\n";
    GenerationParams params;
    params.max_tokens = 48;
    params.temperature = 0.0f;

    auto r = ctx.orchestrator->generate({u}, params, tier_name);

    std::printf("\n===gh106 MTP route===\n"
                "tier=%s drafted=%d accepted=%d finish=%s\n[%s]\n===\n",
                tier_name.c_str(), r.n_drafted, r.n_accepted,
                r.finish_reason.c_str(), r.content.c_str());
    INFO("drafted=" << r.n_drafted << " accepted=" << r.n_accepted);

    REQUIRE(r.error_code == 0);
    REQUIRE_FALSE(r.content.empty());
    // The decisive combined-path gate: the orchestrator actually dispatched to
    // the MTP kernel (n_drafted is only non-zero when generate_mtp ran).
    REQUIRE(r.n_drafted > 0);
    CHECK(r.n_accepted > 0);
}
