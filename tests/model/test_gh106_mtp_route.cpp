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
    // gh#108 (v2.9.3): flash+q4 KV both work now (extern/llama.cpp past upstream
    // #25148); left flash-off/f16 here since this test's purpose is orchestrator
    // routing/wiring, not perf — see test_gh108_mtp_guards.cpp for the flash case.
    tier.flash_attn = false;
    tier.cache_type_k = "f16";
    tier.cache_type_v = "f16";
    tier.grammar.reset();        // greedy, unconstrained — the MTP envelope
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

TEST_CASE("gh#108 MTP engages with tools staged through orchestrator (agent loop)",
          "[model][gh108][mtp][route][tools]") {
    // The reachability fix: the agent loop stages tools, and v2.9.2 no longer
    // refuses MTP for tooled tiers (MTP+tools is lossless-correct). So a tooled
    // generate through the orchestrator must ENGAGE MTP, not loud-fail.
    ModelTestContext ctx;
    auto tier_name = configure_mtp_route(ctx);
    if (!init_orchestrator(ctx)) {
        SKIP("orchestrator init failed (resource/config) — route untested");
    }

    entropic::Message u;
    u.role = "user";
    u.content = "You must call the read_file tool to read /etc/hostname.";
    auto params = greedy_params();
    // gh#108: 200 was too tight once tools actually render (was masked by the
    // schema bug below never staging a real tool at all) — gemma4's thinking
    // chain can exceed 200 before reaching the call, leaving content empty.
    params.max_tokens = 500;
    // gh#108 fix: GenerationParams.tools expects entropic's native MCP shape
    // {name, description, inputSchema} (see mcp_tools_to_common_chat in
    // llama_cpp_backend.cpp) — NOT OpenAI's {type:"function", function:{...}}
    // wire format. The latter silently parses to 0 tools (t.value("name","")
    // finds nothing at the top level, every entry gets skipped), so this call
    // previously rendered tool-LESS despite staging 154 bytes of JSON.
    params.tools = R"([{"name":"read_file","description":"Read a file",)"
                   R"("inputSchema":{"type":"object","properties":{"path":)"
                   R"({"type":"string"}},"required":["path"]}}])";

    auto r = ctx.orchestrator->generate({u}, params, tier_name);
    std::printf("\n===gh108 MTP route + tools===\ncode=%d drafted=%d calls=%zu\n[%s]\n===\n",
                r.error_code, r.n_drafted, r.tool_calls.size(), r.content.c_str());

    // The reachability gate: MTP ENGAGED through the tooled orchestrator path
    // instead of loud-failing (v2.9.1 would have returned code 54 here). The
    // orchestrator parses the MTP result via apply_adapter_parse exactly as for
    // plain decode (MTP is lossless → identical content → identical parse).
    // gh#108 (v2.9.3): previously hedged as "tool FORMAT too unreliable to
    // hard-assert" — root-caused to this test staging OpenAI-shaped JSON
    // against an API that expects entropic's native MCP shape, silently
    // rendering tool-LESS every time. Fixed (see params.tools above); now a
    // real tool call reliably parses, so this asserts it directly.
    REQUIRE(r.error_code == 0);  // NOT a loud refusal — tools are allowed under MTP
    REQUIRE(r.n_drafted > 0);    // MTP actually ran with tools staged
    REQUIRE(r.tool_calls.size() == 1);
    CHECK(r.tool_calls[0].name == "read_file");
}
