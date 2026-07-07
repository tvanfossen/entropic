// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh108_cpu_feasibility.cpp
 * @brief gh#108 follow-up: CPU-only (gpu_layers=0) feasibility check for
 *        E2B-QAT-Q4 + MTP + flash + q4 KV, simulating an embedded ARM SoC
 *        (RK3588-class: no CUDA, unified memory, ~4-8GB total RAM) on this
 *        laptop's CPU. NOT the Vulkan/Mali backend that would actually be
 *        preferred on such hardware — this measures the CPU floor.
 *
 * Same 6-turn scripted conversation as test_gh108_agentic_benchmark.cpp,
 * restricted to the 4 configs relevant to the embedded-feasibility question
 * (E2B-QAT-Q4 x MTP on/off x flash+q4KV on/off), with gpu_layers=0 so
 * llama.cpp runs the full model on CPU (ARM NEON on a real SoC; here,
 * whatever this laptop's CPU is — the point is "no GPU offload available",
 * which is the shared property with an RK3588 lacking a working GPU
 * backend for this stack).
 *
 * Tracks process RSS (not VRAM — there is no VRAM in this scenario) via
 * /proc/self/status, sampled right after model activation, to check
 * against an assumed 8GB total-system budget.
 *
 * v2.9.4: max_tokens matches test_gh108_agentic_benchmark.cpp (1500), not a
 * CPU-reduced budget as originally set at 300. The 300 figure was root-caused
 * as the actual cause of empty-content failures on post-tool-answer turns —
 * tokens needed to resolve tool-result ambiguity is a function of model
 * reasoning length, not decode speed, so shrinking the budget for CPU speed
 * just truncates the same answer earlier. This makes the CPU run slower;
 * that's the accepted cost of not silently truncating real answers.
 */

#include "model_test_context.h"  // helpers only — NO CATCH_REGISTER_LISTENER

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

std::filesystem::path models_dir() {
    return std::filesystem::path(getenv("HOME")) / ".entropic" / "models";
}

// Current resident memory of THIS process (MiB), via /proc/self/status.
// Not VRAM — there is none in the CPU-only scenario this file simulates.
long query_rss_mb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            long kb = 0;
            iss >> kb;
            return kb / 1024;
        }
    }
    return -1;
}

// gh#108 fix: native MCP shape, not OpenAI's — see
// test_gh108_agentic_benchmark.cpp's kTools comment for the full story.
const char* kTools =
    R"([{"name":"get_weather","description":"Get current weather for a city",)"
    R"("inputSchema":{"type":"object","properties":{"location":)"
    R"({"type":"string"}},"required":["location"]}},)"
    R"({"name":"search_notes","description":"Search the user's saved notes",)"
    R"("inputSchema":{"type":"object","properties":{"query":)"
    R"({"type":"string"}},"required":["query"]}}])";

struct TurnResult {
    std::string label;
    int tokens = 0;
    double wall_ms = 0.0;
    double decode_tok_s = 0.0;
    bool tool_called = false;
    std::string content;
};

struct BenchConfig {
    std::string label;
    bool mtp;
    bool flash_q4kv;
};

std::string configure_tier(ModelTestContext& ctx, const std::filesystem::path& path,
                           const std::filesystem::path& head, const BenchConfig& cfg) {
    REQUIRE(load_registry(ctx.registry));
    REQUIRE(load_test_config(ctx.registry, ctx.config));
    auto tier_name = ctx.config.models.default_tier;
    auto it = ctx.config.models.tiers.find(tier_name);
    if (it == ctx.config.models.tiers.end()) {
        SKIP("no default tier in config to repoint");
    }
    auto& tier = it->second;
    tier.path = path;
    tier.adapter = "gemma4";
    tier.gpu_layers = 0;  // CPU-only — the embedded-SoC-without-GPU-offload case
    tier.context_length = 131072;  // true 128k, the RK3588 question's premise
    tier.flash_attn = cfg.flash_q4kv;
    tier.cache_type_k = cfg.flash_q4kv ? "q4_0" : "f16";
    tier.cache_type_v = cfg.flash_q4kv ? "q4_0" : "f16";
    tier.grammar.reset();
    auto& spec = ctx.config.inference.speculative;
    spec.enabled = cfg.mtp;
    spec.mtp = cfg.mtp;
    if (cfg.mtp) {
        spec.draft.path = head;
        spec.n_draft = 4;
    }
    return tier_name;
}

// Mirrors init_orchestrator minus the 16K context clamp — see
// test_gh108_agentic_benchmark.cpp for the full rationale.
bool init_orchestrator_full_ctx(ModelTestContext& ctx) {
    ctx.orchestrator = std::make_unique<ModelOrchestrator>();
    if (!ctx.orchestrator->initialize(ctx.config)) {
        spdlog::error("Orchestrator init failed");
        return false;
    }
    if (ctx.orchestrator->grammar_registry().size() == 0) {
        ctx.orchestrator->load_grammars_from(
            std::filesystem::path(MODEL_PATH) / "data" / "grammars");
    }
    ctx.default_tier = ctx.config.models.default_tier;
    ctx.initialized = true;
    return true;
}

// gh#108 (v2.9.4): max_tokens raised 300 -> 1500 (matches
// test_gh108_agentic_benchmark.cpp's fix). The original 300 was chosen for
// CPU-speed reasons, not correctness — but 300 was root-caused as the actual
// cause of "empty content" failures on post-tool-answer turns (E2B needs
// ~713 tokens to work through tool-result ambiguity when tools stay staged;
// token count needed is a function of model reasoning length, not decode
// speed, so a CPU-specific reduced budget just truncates the same answer
// earlier). This makes the CPU run noticeably slower; that's the accepted
// cost of not silently truncating real answers.
TurnResult run_turn(ModelOrchestrator& orch, std::vector<entropic::Message>& conv,
                    const std::string& tier_name, const char* label) {
    entropic::GenerationParams params;
    params.max_tokens = 1500;
    params.temperature = 0.0f;
    params.tools = kTools;

    auto t0 = std::chrono::steady_clock::now();
    auto r = orch.generate(conv, params, tier_name);
    auto t1 = std::chrono::steady_clock::now();

    TurnResult tr;
    tr.label = label;
    tr.tokens = r.token_count;
    tr.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    tr.decode_tok_s = r.throughput_tok_s;
    tr.content = r.content;

    entropic::Message assistant;
    assistant.role = "assistant";
    assistant.content = r.content;
    conv.push_back(assistant);

    if (!r.tool_calls.empty()) {
        tr.tool_called = true;
        std::string canned = (r.tool_calls[0].name == "get_weather")
            ? "18C, partly cloudy"
            : "Q3 budget note: increase marketing spend by 12%.";
        entropic::Message tool_result;
        tool_result.role = "user";
        tool_result.content = canned;
        tool_result.metadata["tool_call_id"] = r.tool_calls[0].id;
        tool_result.metadata["tool_name"] = r.tool_calls[0].name;
        conv.push_back(tool_result);
    }

    // gh#108 (v2.9.4): a tool-call-only turn (no narrative preamble) is a
    // legitimate, well-behaved response — content is correctly empty then
    // (the answer lives in tool_calls, not content). Only flag a turn that
    // produced neither.
    CHECK(r.error_code == 0);
    CHECK_FALSE((r.content.empty() && r.tool_calls.empty()));
    return tr;
}

std::vector<TurnResult> run_agentic_script(ModelOrchestrator& orch,
                                           const std::string& tier_name) {
    std::vector<TurnResult> stats;
    std::vector<entropic::Message> conv;
    auto user_turn = [&](const char* text) {
        entropic::Message u;
        u.role = "user";
        u.content = text;
        conv.push_back(u);
    };

    user_turn("What's the weather in Paris? Use the get_weather tool.");
    stats.push_back(run_turn(orch, conv, tier_name, "T1"));
    if (stats.back().tool_called) stats.push_back(run_turn(orch, conv, tier_name, "T1b"));

    user_turn("Also check the weather in Tokyo.");
    stats.push_back(run_turn(orch, conv, tier_name, "T2"));
    if (stats.back().tool_called) stats.push_back(run_turn(orch, conv, tier_name, "T2b"));

    user_turn("Compare the two cities' weather in one sentence.");
    stats.push_back(run_turn(orch, conv, tier_name, "T3"));

    user_turn("Search my notes for 'Q3 budget' using search_notes.");
    stats.push_back(run_turn(orch, conv, tier_name, "T4"));
    if (stats.back().tool_called) stats.push_back(run_turn(orch, conv, tier_name, "T4b"));

    user_turn("Given the budget note, and remembering both weather reports, "
             "write a 2-sentence trip-planning recommendation.");
    stats.push_back(run_turn(orch, conv, tier_name, "T5"));

    user_turn("Now call get_weather for London, then summarize all three "
             "cities' weather in one short paragraph.");
    stats.push_back(run_turn(orch, conv, tier_name, "T6"));
    if (stats.back().tool_called) stats.push_back(run_turn(orch, conv, tier_name, "T6b"));

    return stats;
}

double avg_decode_tok_s(const std::vector<TurnResult>& stats) {
    if (stats.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& t : stats) sum += t.decode_tok_s;
    return sum / static_cast<double>(stats.size());
}

}  // namespace

TEST_CASE("gh#108 CPU-only feasibility: E2B-QAT-Q4 x MTP x flash+q4KV, "
          "128k context, RSS vs assumed 8GB budget",
          "[model][gh108][benchmark][cpu][feasibility]") {
    auto path = models_dir() / "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf";
    auto head = models_dir() / "mtp-gemma-4-E2B-it.gguf";
    if (!std::filesystem::is_regular_file(path) || !std::filesystem::is_regular_file(head)) {
        SKIP("E2B-QAT-Q4 GGUF/head not present");
    }

    std::vector<BenchConfig> configs = {
        {"E2B-QAT-Q4 -MTP -flash(f16KV) [CPU]", false, false},
        {"E2B-QAT-Q4 -MTP +flash+q4KV [CPU]", false, true},
        {"E2B-QAT-Q4 +MTP -flash(f16KV) [CPU]", true, false},
        {"E2B-QAT-Q4 +MTP +flash+q4KV [CPU]", true, true},
    };

    constexpr long kAssumedBudgetMb = 8192;
    struct Summary { std::string label; long rss_mb; double avg_tok_s; };
    std::vector<Summary> summaries;

    for (const auto& cfg : configs) {
        ModelTestContext ctx;
        auto tier = configure_tier(ctx, path, head, cfg);
        if (!init_orchestrator_full_ctx(ctx)) {
            spdlog::error("orchestrator init failed for {} — skipping", cfg.label);
            continue;
        }
        long rss = query_rss_mb();
        std::printf("\n================ %s (RSS=%ld MiB, budget=%ld MiB, fits=%s) "
                   "================\n",
                   cfg.label.c_str(), rss, kAssumedBudgetMb,
                   (rss > 0 && rss < kAssumedBudgetMb) ? "YES" : "NO/UNKNOWN");
        auto stats = run_agentic_script(*ctx.orchestrator, tier);
        for (const auto& t : stats) {
            std::printf("-- %s -- tokens=%d wall_ms=%.1f decode_tok/s=%.2f tool=%s\n"
                       "content:\n%s\n",
                       t.label.c_str(), t.tokens, t.wall_ms, t.decode_tok_s,
                       t.tool_called ? "yes" : "no", t.content.c_str());
        }
        double avg = avg_decode_tok_s(stats);
        summaries.push_back({cfg.label, rss, avg});
        for (const auto& t : stats) {
            INFO(cfg.label << " " << t.label);
            // gh#108 (v2.9.4): tool-call-only turns legitimately have empty
            // content — see the matching comment in run_turn().
            CHECK_FALSE((t.content.empty() && !t.tool_called));
        }
    }

    std::printf("\n================ CPU FEASIBILITY SUMMARY "
               "(assumed budget=%ld MiB) ================\n", kAssumedBudgetMb);
    std::printf("%-42s %10s %12s %8s\n", "config", "RSS MiB", "avg tok/s", "fits8G");
    for (const auto& s : summaries) {
        std::printf("%-42s %10ld %12.2f %8s\n",
                   s.label.c_str(), s.rss_mb, s.avg_tok_s,
                   (s.rss_mb > 0 && s.rss_mb < kAssumedBudgetMb) ? "yes" : "NO");
    }
}
