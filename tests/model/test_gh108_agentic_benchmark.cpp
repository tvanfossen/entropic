// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh108_agentic_benchmark.cpp
 * @brief gh#108 (v2.9.3) follow-up: full permutation matrix — 6 weight
 *        variants (E4B/E2B x {Q8, UD-Q4, QAT-Q4}) x {MTP off/on} x
 *        {flash+q4KV off/on} = 24 configs, at true 128k context, over the
 *        same scripted 6-turn tool-calling conversation for every config.
 *
 * Adds a deterministic, keyword-recall QUALITY score per config (0-9) on
 * top of the performance numbers — the single-turn/first agentic passes
 * only reported speed; this closes that gap with a cheap, transparent,
 * reproducible proxy (does the final answer at each script step actually
 * recall the right entity — city name, budget note, etc.) rather than an
 * LLM-judge call (no extra inference cost, no judge-model variance).
 *
 * Matrix axes, independently toggled per config:
 *   - weight quant: Q8_0, UD-Q4_K_XL (post-training dynamic), qat-UD-Q4_K_XL
 *     (quantization-aware trained)
 *   - model size: E4B, E2B
 *   - MTP: off / on (target-owned head, per-family)
 *   - flash+q4 KV: off (flash off, f16 KV) / on (flash on, q4_0 KV) —
 *     quantized KV requires flash in llama.cpp, so these two toggle together
 *
 * NOT a regression gate — functional assertions only. Full per-config
 * report + a final summary table (speed avg, VRAM, quality score) are
 * printed for human review.
 */

#include "model_test_context.h"  // helpers only — NO CATCH_REGISTER_LISTENER

// gh#142: the same estimator the engine's admission gate uses, so the benchmark
// cannot ask for a configuration the gate would refuse.
#include "device_memory.h"
#include "vram_footprint.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {

std::filesystem::path models_dir() {
    return std::filesystem::path(getenv("HOME")) / ".entropic" / "models";
}

long query_vram_used_mb() {
    FILE* pipe = popen(
        "nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits -i 0", "r");
    if (!pipe) return -1;
    char buf[64] = {0};
    bool ok = std::fgets(buf, sizeof(buf), pipe) != nullptr;
    pclose(pipe);
    return ok ? std::strtol(buf, nullptr, 10) : -1;
}

bool icontains(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != hay.end();
}

// gh#108 fix: GenerationParams.tools expects entropic's native MCP shape
// {name, description, inputSchema} (see mcp_tools_to_common_chat in
// llama_cpp_backend.cpp) — NOT OpenAI's {type:"function", function:{...}}
// wire format. The previous OpenAI-shaped JSON here silently parsed to 0
// tools (t.value("name","") finds nothing at the top level, every entry
// gets skipped), so every "tool call expected" turn in this whole matrix
// ran tool-LESS the entire time — the model was hallucinating call-shaped
// text from the prompt wording alone, never given a real tool schema.
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
    std::string tool_name;
    std::string content;
    int error_code = 0;
    std::string error_message;
    std::string finish_reason;
};

struct ModelVariant {
    std::string label;          // e.g. "E4B-Q8"
    std::filesystem::path path;
    std::filesystem::path head; // per-family MTP head
};

struct BenchConfig {
    std::string label;
    ModelVariant variant;
    bool mtp;
    bool flash_q4kv;  // flash_attn + q4_0 KV together; off = flash off + f16 KV
};

/// The context this matrix was written to exercise, when the card allows it.
constexpr int kDesiredContext = 131072;

/// Below this the agentic script cannot hold its own transcript, so a run would
/// measure truncation rather than throughput. Reported, not measured.
constexpr int kMinUsefulContext = 8192;

/**
 * @brief Largest context this config can actually run at on THIS device.
 *
 * gh#142: asks the same estimator the engine's admission gate uses, so the
 * benchmark and the gate cannot disagree about what fits — a benchmark that
 * requested more than the gate admits would simply be refused, and one that
 * requested more than the DEVICE has used to abort the process.
 *
 * Falls back to the full desired context when free VRAM cannot be determined
 * (CPU build, no GPU): there is no device budget to fit inside, and the old
 * unconditional behaviour is the right one there.
 *
 * @param tier Tier being configured, already carrying path and cache types.
 * @param cfg Benchmark config, for the log line.
 * @return Context length in tokens; 0 when nothing fits.
 * @version 2.11.0
 */
int fit_context_length(const entropic::ModelConfig& tier, const BenchConfig& cfg) {
    const uint64_t available = entropic::query_device_free_vram_bytes();
    if (available == 0) {
        return kDesiredContext;
    }
    std::error_code ec;
    auto weights = std::filesystem::file_size(tier.path, ec);
    if (ec) {
        return kDesiredContext;
    }
    entropic::FootprintInputs in;
    in.weights_bytes = weights;
    in.mmproj_bytes = 0;  // cleared above — this benchmark is text-only
    in.gpu_layers = tier.gpu_layers;
    in.context_length = kDesiredContext;
    in.cache_type_k = tier.cache_type_k;
    in.cache_type_v = tier.cache_type_v;
    // gh#142: NOT the 512 MiB default. recommend_context_length returns the
    // LARGEST context that fits, so whatever reserve it is given is the only
    // headroom left — and llama.cpp's per-context compute buffers, which the
    // estimate cannot price, measured 1030-2354 MiB in this very benchmark.
    // Budgeting 512 meant aiming to consume the entire card and then failing
    // on the graph reservation. Size the reserve from the measured worst case
    // so the fit scales to a context with real headroom instead.
    in.vram_reserve_mb = 2560;
    const int fits = entropic::recommend_context_length(in, available);
    std::printf("[fit] %-32s weights=%.2f GiB free=%.2f GiB -> context=%d%s\n",
                cfg.label.c_str(),
                static_cast<double>(weights) / 1073741824.0,
                static_cast<double>(available) / 1073741824.0,
                fits,
                fits == kDesiredContext ? " (full 128k)" : " (reduced to fit)");
    return fits;
}

/**
 * @brief Halve a config's context in place for a retry after a refusal.
 *
 * gh#142: the estimate cannot price compute buffers, so its first answer can be
 * too generous. Halving is empirical rather than derived — it needs no model
 * metadata, and one step usually clears the ~1-2 GiB the estimate missed.
 *
 * @param ctx Test context whose tier is re-pointed.
 * @param tier_name Tier to adjust.
 * @return The new context length.
 * @version 2.11.0
 */
int halve_context(ModelTestContext& ctx, const std::string& tier_name) {
    auto& tier = ctx.config.models.tiers[tier_name];
    tier.context_length /= 2;
    return tier.context_length;
}

// Builds a tier for one config. Context is sized to the device (gh#142);
// bypasses the shared test-harness 16K safety clamp — see
// init_orchestrator_full_ctx below.
std::string configure_tier(ModelTestContext& ctx, const BenchConfig& cfg) {
    REQUIRE(load_registry(ctx.registry));
    REQUIRE(load_test_config(ctx.registry, ctx.config));
    auto tier_name = ctx.config.models.default_tier;
    auto it = ctx.config.models.tiers.find(tier_name);
    if (it == ctx.config.models.tiers.end()) {
        SKIP("no default tier in config to repoint");
    }
    auto& tier = it->second;
    tier.path = cfg.variant.path;
    tier.adapter = "gemma4";
    tier.gpu_layers = 99;
    tier.flash_attn = cfg.flash_q4kv;
    tier.cache_type_k = cfg.flash_q4kv ? "q4_0" : "f16";
    tier.cache_type_v = cfg.flash_q4kv ? "q4_0" : "f16";
    tier.grammar.reset();
    // gh#142: the default tier inherits `mmproj: primary_mmproj` from the
    // global config, which resolves to the QWEN3.6-35B projector. Repointing
    // `path` at a gemma-4 model left that MISMATCHED projector attached, so
    // every config loaded 857.6 MiB of the wrong model's vision tower into an
    // agentic TEXT benchmark. That is the allocation whose cudaMalloc failed
    // and took the process down via GGML_ASSERT(buffer). Nothing here measures
    // vision; drop it.
    tier.mmproj_path.clear();
    // gh#142: context is sized to the card at run time rather than demanding a
    // fixed 128k. The original "true 128k, max VRAM contention" ask cannot be
    // satisfied on an 11 GB device by the E4B variants at full offload, and the
    // engine had no working budget gate to refuse it — so it aborted instead.
    // Each config now reports the context it actually ran at.
    tier.context_length = fit_context_length(tier, cfg);
    auto& spec = ctx.config.inference.speculative;
    spec.enabled = cfg.mtp;
    spec.mtp = cfg.mtp;
    if (cfg.mtp) {
        spec.draft.path = cfg.variant.head;
        spec.n_draft = 4;
    }
    return tier_name;
}

// Mirrors model_test_context.h's init_orchestrator, MINUS the 16K
// context_length clamp and partial-gpu-layers clamp — this file needs the
// real 131072 context to land, and these GGUFs (~2.6-8.2GB) are well under
// the >10GB threshold that clamp exists for anyway. Deliberately NOT
// touching the shared init_orchestrator (other tests rely on its clamps for
// dev-box memory safety); this is scoped to this file's explicit 128k ask.
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

// One orchestrator call + timing, with tool-call detection.
//
// gh#108 (v2.9.4): max_tokens raised 700 -> 8000 across several iterations,
// root-caused to a real budget-truncation class of bug (tools stay staged on
// every turn, so a tool-result message leaves E2B genuinely uncertain
// whether it needs another tool call — it works through that via a long,
// self-correcting reasoning chain that can run past a too-small budget).
// But 8000 does NOT deterministically eliminate every empty-turn case:
// repeated full-matrix runs at identical max_tokens showed DIFFERENT configs
// failing run-to-run at IDENTICAL settings — greedy decode on this stack is
// not perfectly reproducible (matches an independent finding: the same
// single call repeated at temperature=0 produced 15/155/177-token outputs
// across separate runs, from floating-point non-associativity in
// batched/threaded compute, not intentional sampling). Temperature was
// originally pinned to 0.0 specifically for that assumed reproducibility —
// which turned out false on this stack, so it was buying nothing. Switched
// to the engine's real default (0.7, config.h) instead: this is what
// production tiers actually run at, and it's the exact capability gh#108
// v2.9.4 added (MTP now works correctly at temperature>0) — a benchmark
// that only ever ran at temperature=0 never exercised it. A genuinely empty
// turn (no content, no tool call) is retried before being treated as a
// failure — real production agent loops need this resilience regardless of
// temperature or this test.
TurnResult run_turn(ModelOrchestrator& orch, std::vector<entropic::Message>& conv,
                    const std::string& tier_name, const char* label) {
    entropic::GenerationParams params;
    params.max_tokens = 8000;
    params.temperature = 0.7f;  // engine default (config.h) — was 0.0f
    params.tools = kTools;

    entropic::GenerationResult r;
    double wall_ms = 0.0;
    constexpr int kMaxAttempts = 3;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto t0 = std::chrono::steady_clock::now();
        r = orch.generate(conv, params, tier_name);
        auto t1 = std::chrono::steady_clock::now();
        wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (r.error_code != 0 || !r.content.empty() || !r.tool_calls.empty()) {
            break;
        }
        // Genuinely empty turn (no content, no tool call) — the same
        // nondeterminism that produced it can produce a real answer on
        // retry (confirmed empirically: repeat calls at temperature=0 vary).
        std::printf("[gh108] %s: empty turn on attempt %d/%d, retrying\n",
                   label, attempt + 1, kMaxAttempts);
    }

    TurnResult tr;
    tr.label = label;
    tr.tokens = r.token_count;
    tr.wall_ms = wall_ms;
    tr.decode_tok_s = r.throughput_tok_s;
    tr.content = r.content;
    tr.error_code = r.error_code;
    tr.error_message = r.error_message;
    tr.finish_reason = r.finish_reason;

    entropic::Message assistant;
    assistant.role = "assistant";
    assistant.content = r.content;
    conv.push_back(assistant);

    if (!r.tool_calls.empty()) {
        tr.tool_called = true;
        tr.tool_name = r.tool_calls[0].name;
        std::string canned;
        if (r.tool_calls[0].name == "get_weather") {
            canned = "18C, partly cloudy";
        } else if (r.tool_calls[0].name == "search_notes") {
            canned = "Q3 budget note: increase marketing spend by 12%.";
        } else {
            canned = "ok";
        }
        entropic::Message tool_result;
        tool_result.role = "user";
        tool_result.content = canned;
        tool_result.metadata["tool_call_id"] = r.tool_calls[0].id;
        tool_result.metadata["tool_name"] = r.tool_calls[0].name;
        conv.push_back(tool_result);
    }

    // Non-fatal: an empty/errored turn IS a data point this benchmark exists
    // to surface, not a harness bug to crash on.
    // gh#108 (v2.9.4): a turn that answers with ONLY a tool call (no
    // narrative preamble) is a legitimate, well-behaved response — content
    // is correctly empty in that case (the answer lives in tool_calls, not
    // content). Only flag a turn that produced neither, after retries.
    // gh#142: ENTROPIC_ERROR_LOAD_FAILED here means the device could not
    // satisfy an allocation this config needs — measured case: the MTP head's
    // context wanted 1222 MiB of COMPUTE BUFFERS (graph reservation scratch)
    // that no pre-flight estimate can predict, because the size depends on
    // model internals the estimate deliberately does not read. The engine
    // reported it as a typed error instead of aborting, which is the whole
    // point of the gh#142 work. Recording "this config does not fit this
    // device" is the honest result; asserting error_code == 0 would be
    // asserting something false about the HARDWARE, not about the code.
    // Every OTHER error code is still a real failure and still asserted.
    const bool resource_refusal = (r.error_code == ENTROPIC_ERROR_LOAD_FAILED);
    if (!resource_refusal) {
        CHECK(r.error_code == 0);
        CHECK_FALSE((r.content.empty() && r.tool_calls.empty()));
    }
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
    stats.push_back(run_turn(orch, conv, tier_name, "T1 (tool call expected)"));
    if (stats.back().tool_called) {
        stats.push_back(run_turn(orch, conv, tier_name, "T1b (post-tool answer)"));
    }

    user_turn("Also check the weather in Tokyo.");
    stats.push_back(run_turn(orch, conv, tier_name, "T2 (tool call expected)"));
    if (stats.back().tool_called) {
        stats.push_back(run_turn(orch, conv, tier_name, "T2b (post-tool answer)"));
    }

    user_turn("Compare the two cities' weather in one sentence.");
    stats.push_back(run_turn(orch, conv, tier_name, "T3 (text only)"));

    user_turn("Search my notes for 'Q3 budget' using search_notes.");
    stats.push_back(run_turn(orch, conv, tier_name, "T4 (tool call expected)"));
    if (stats.back().tool_called) {
        stats.push_back(run_turn(orch, conv, tier_name, "T4b (post-tool answer)"));
    }

    user_turn("Given the budget note, and remembering both weather reports, "
             "write a 2-sentence trip-planning recommendation.");
    stats.push_back(run_turn(orch, conv, tier_name, "T5 (long-context recall)"));

    user_turn("Now call get_weather for London, then summarize all three "
             "cities' weather in one short paragraph.");
    stats.push_back(run_turn(orch, conv, tier_name, "T6 (tool call expected)"));
    if (stats.back().tool_called) {
        stats.push_back(run_turn(orch, conv, tier_name, "T6b (final synthesis)"));
    }

    return stats;
}

// Parses the leading step number out of a label like "T3 (text only)" or
// "T4b (post-tool answer)" -> 3, 4.
int step_of(const std::string& label) {
    size_t i = 1;  // skip leading 'T'
    std::string digits;
    while (i < label.size() && std::isdigit(static_cast<unsigned char>(label[i]))) {
        digits += label[i++];
    }
    return digits.empty() ? -1 : std::stoi(digits);
}

// Deterministic keyword-recall rubric — max 9 points total across 6 steps.
// Not a proxy for eloquence; a proxy for "did the answer contain the right
// entity at the right step." Scored against whichever call in that step's
// group is LAST in the transcript (the "b" follow-up if one exists, else
// the initial attempt) — that's the call most likely to hold the real answer.
int score_step(int step, const std::string& content) {
    switch (step) {
        case 1: return icontains(content, "Paris") ? 1 : 0;
        case 2: return icontains(content, "Tokyo") ? 1 : 0;
        case 3: return (icontains(content, "Paris") && icontains(content, "Tokyo")) ? 1 : 0;
        case 4: return (icontains(content, "budget") || icontains(content, "Q3")) ? 1 : 0;
        case 5: {
            int pts = 0;
            if (icontains(content, "Paris")) pts++;
            if (icontains(content, "Tokyo")) pts++;
            if (icontains(content, "budget")) pts++;
            return pts;
        }
        case 6: {
            int pts = 0;
            if (icontains(content, "London")) pts++;
            if (icontains(content, "Paris") && icontains(content, "Tokyo") &&
                icontains(content, "London")) pts++;
            return pts;
        }
        default: return 0;
    }
}
constexpr int kMaxQualityScore = 9;

int score_config(const std::vector<TurnResult>& stats) {
    std::unordered_map<int, const TurnResult*> last_by_step;
    for (const auto& t : stats) {
        int s = step_of(t.label);
        if (s > 0) last_by_step[s] = &t;  // later entries overwrite — "b" wins
    }
    int total = 0;
    for (auto& [step, t] : last_by_step) total += score_step(step, t->content);
    return total;
}

double avg_decode_tok_s(const std::vector<TurnResult>& stats) {
    if (stats.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& t : stats) sum += t.decode_tok_s;
    return sum / static_cast<double>(stats.size());
}

void print_report(const char* config_label, long vram_mb,
                  const std::vector<TurnResult>& stats) {
    std::printf("\n================ %s (VRAM active=%ld MiB) ================\n",
               config_label, vram_mb);
    for (const auto& t : stats) {
        std::printf(
            "-- %s --\n"
            "  tokens=%d wall_ms=%.1f decode_tok/s=%.2f tool_called=%s%s%s finish=%s\n"
            "%s%s%s"
            "  content:\n%s\n",
            t.label.c_str(), t.tokens, t.wall_ms, t.decode_tok_s,
            t.tool_called ? "YES(" : "no",
            t.tool_called ? t.tool_name.c_str() : "",
            t.tool_called ? ")" : "",
            t.finish_reason.c_str(),
            // A failed turn without its reason is an unactionable data point.
            t.error_message.empty() ? "" : "  error: ",
            t.error_message.c_str(),
            t.error_message.empty() ? "" : "\n",
            t.content.c_str());
    }
}

}  // namespace

TEST_CASE("gh#108 agentic benchmark: full 24-config permutation matrix "
          "(6 weight variants x MTP x flash+q4KV) at true 128k context",
          "[.][model][gh108][benchmark][agentic][matrix]") {
    auto md = models_dir();
    // v2.14.6: the pared model set (user, 2026-10-17) — one row per model
    // still on disk, each trunk with ITS OWN head.
    std::vector<ModelVariant> variants = {
        {"E4B-QAT-Q4", md / "gemma-4-E4B-it-qat-UD-Q4_K_XL.gguf", md / "mtp-gemma-4-E4B-it.gguf"},
        {"E2B-Q8", md / "gemma-4-E2B-it-Q8_0.gguf", md / "mtp-gemma-4-E2B-it.gguf"},
    };
    for (const auto& v : variants) {
        if (!std::filesystem::is_regular_file(v.path) ||
            !std::filesystem::is_regular_file(v.head)) {
            SKIP("missing GGUF for variant " + v.label + " (" + v.path.string() + ")");
        }
    }

    std::vector<BenchConfig> configs;
    for (const auto& v : variants) {
        for (bool mtp : {false, true}) {
            for (bool flash_q4kv : {false, true}) {
                std::string label = v.label + (mtp ? " +MTP" : " -MTP") +
                                    (flash_q4kv ? " +flash+q4KV" : " -flash(f16KV)");
                configs.push_back({label, v, mtp, flash_q4kv});
            }
        }
    }

    struct Summary { std::string label; long vram; double avg_tok_s; int quality; };
    std::vector<Summary> summaries;

    int measured = 0;
    int did_not_fit = 0;

    for (const auto& cfg : configs) {
        ModelTestContext ctx;
        auto tier = configure_tier(ctx, cfg);
        // gh#142: a config that does not fit the card AS IT IS RIGHT NOW is a
        // measured result, not a skipped test — the whole point of adapting to
        // hardware is that the answer depends on what else holds VRAM. Report
        // it and move to the next config rather than attempting a load the
        // device cannot satisfy, which is what used to abort the process.
        const int fitted = ctx.config.models.tiers[tier].context_length;
        if (fitted < kMinUsefulContext) {
            std::printf("[fit] %-32s DOES NOT FIT — needs >= %d ctx, "
                        "device offers %d. Free VRAM, or use a smaller "
                        "quantization.\n",
                        cfg.label.c_str(), kMinUsefulContext, fitted);
            ++did_not_fit;
            continue;
        }
        if (!init_orchestrator_full_ctx(ctx)) {
            spdlog::error("orchestrator init failed for {} — skipping", cfg.label);
            continue;
        }
        ++measured;
        long vram = query_vram_used_mb();
        auto stats = run_agentic_script(*ctx.orchestrator, tier);
        print_report(cfg.label.c_str(), vram, stats);

        // gh#142: a resource refusal is NOT a verdict on the config. The
        // estimate cannot price compute buffers (measured 1030-2354 MiB per
        // context against a 512 MiB default reserve), so it can hand back a
        // context the device then refuses. The honest response is to back off
        // and measure what DOES run, not to declare the config unfittable —
        // it fits at a smaller context, and on a less contended card it may
        // fit at the original one.
        const bool refused = !stats.empty()
            && std::all_of(stats.begin(), stats.end(), [](const TurnResult& t) {
                   return t.error_code == ENTROPIC_ERROR_LOAD_FAILED;
               });
        if (refused) {
            const int retry_ctx = halve_context(ctx, tier);
            if (retry_ctx >= kMinUsefulContext) {
                std::printf("[fit] %-32s refused at ctx=%d — retrying at %d\n",
                            cfg.label.c_str(), fitted, retry_ctx);
                if (!init_orchestrator_full_ctx(ctx)) {
                    spdlog::error("re-init failed for {}", cfg.label);
                    ++did_not_fit;
                    --measured;
                    continue;
                }
                vram = query_vram_used_mb();
                stats = run_agentic_script(*ctx.orchestrator, tier);
                print_report(cfg.label.c_str(), vram, stats);
            }
        }
        const bool still_refused = !stats.empty()
            && std::all_of(stats.begin(), stats.end(), [](const TurnResult& t) {
                   return t.error_code == ENTROPIC_ERROR_LOAD_FAILED;
               });
        if (still_refused) {
            std::printf("[fit] %-32s could not be measured: the device refused "
                        "its allocations even after backing off. Free VRAM is "
                        "sampled once and other processes share this card, so "
                        "this is a reading of THIS run, not a verdict on the "
                        "config.\n", cfg.label.c_str());
            ++did_not_fit;
            --measured;
            continue;
        }
        double avg = avg_decode_tok_s(stats);
        int quality = score_config(stats);
        summaries.push_back({cfg.label, vram, avg, quality});
        for (const auto& t : stats) {
            INFO(cfg.label << " " << t.label);
            // gh#108 (v2.9.4): tool-call-only turns legitimately have empty
            // content — see the matching comment in run_turn().
            CHECK_FALSE((t.content.empty() && !t.tool_called));
        }
    }

    std::printf("\n================ FULL MATRIX SUMMARY "
               "(avg decode tok/s, VRAM at fitted ctx, quality /%d) "
               "================\n", kMaxQualityScore);
    std::printf("%d config(s) measured, %d did not fit this device\n",
                measured, did_not_fit);
    std::printf("%-32s %12s %10s %10s\n", "config", "avg tok/s", "VRAM MiB", "quality");
    for (const auto& s : summaries) {
        std::printf("%-32s %12.2f %10ld %10d\n",
                   s.label.c_str(), s.avg_tok_s, s.vram, s.quality);
    }

    // gh#142: adapting to the hardware must not become a way to pass without
    // measuring anything. If every config was reported as unfittable then this
    // run produced no benchmark at all, and saying so is the honest outcome —
    // a green tick over an empty summary table is the vacuous pass this repo
    // has been burned by before.
    INFO("measured=" << measured << " did_not_fit=" << did_not_fit
         << " — free VRAM on the device and re-run; a compositor or a game "
            "holding several GiB is enough to empty this matrix");
    CHECK(measured > 0);
}
