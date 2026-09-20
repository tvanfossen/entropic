// SPDX-License-Identifier: Apache-2.0
/**
 * @file orchestrator.cpp
 * @brief ModelOrchestrator implementation.
 *
 * Model pool deduplication, per-tier adapters, VRAM lifecycle,
 * tier routing via router complete(), swap logic, and grammar
 * registry integration.
 *
 * @version 1.9.3
 */

#include <entropic/inference/orchestrator.h>
#include <entropic/inference/speculative_compat.h>
#include <entropic/interfaces/i_inference_backend.h>
#include <entropic/types/logging.h>

#include "llama_cpp_backend.h"
#include "empty_content_diagnosis.h"
#include "device_memory.h"
#include "vram_footprint.h"
#include "response_parse.h"
#include "grammar_source.h"    // gh#154: provenance for the result record
#include "mtp_envelope.h"
#include <entropic/core/stream_think_filter.h>
#include "adapters/adapter_registry.h"
#include <entropic/inference/adapters/adapter_base.h>  // gh#88 recovery

#include <llama.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.orchestrator");
using entropic::log::now;
using entropic::log::elapsed_ms;

/**
 * @brief Extract latest user message from conversation.
 * @param messages Conversation history.
 * @return Latest user message content.
 * @dg_internal
 * @version 1.8.2
 */
std::string extract_latest_user_message(const std::vector<Message>& messages) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "user") {
            return it->content;
        }
    }
    return "";
}

} // anonymous namespace

// ── Initialization ─────────────────────────────────────────

/**
 * @brief Initialize orchestrator from parsed config.
 *
 * Creates ONE backend per unique model path. Multiple tiers sharing
 * the same .gguf share a single backend instance. Router instantiation
 * moved to SecondaryModelLoader in v2.1.11 (gh#27) — this function no
 * longer touches the router slot directly.
 *
 * @param config Full engine config.
 * @return true on success.
 * @dg_internal
 * @version 2.1.11
 */
bool ModelOrchestrator::create_tier_backends(const ParsedConfig& config) {
    for (const auto& [name, tier_config] : config.models.tiers) {
        std::string path_key = tier_config.path.string();
        if (!std::filesystem::exists(tier_config.path)) {
            logger->error("Model file not found for tier '{}': {}",
                          name, path_key);
            logger->error("Place a GGUF file at the path above, or set "
                          "ENTROPIC_MODEL_DIR to a directory containing "
                          "it. Run `entropic download --list` to see "
                          "bundled model keys, then "
                          "`entropic download <key>` to fetch one.");
            return false;
        }
        if (model_pool_.find(path_key) == model_pool_.end()) {
            model_pool_[path_key] = std::make_shared<LlamaCppBackend>();
        }
        tiers_[name] = model_pool_[path_key];
        adapters_[name] = create_adapter(
            tier_config.adapter, name, "" /* prompt resolved later */);
    }
    // Router backend instantiation moved to SecondaryModelLoader
    // (gh#27, v2.1.11). The loader allocates the role slot lazily on
    // first ensure_loaded() call from activate_router().
    logger->info("Created {} unique backend(s) for {} tier(s)",
                 model_pool_.size(), tiers_.size());
    return true;
}

/**
 * @brief Build digit-to-tier and handoff rule maps from config.
 *
 * An absent `handoff_rules` block leaves the map empty, which is what makes
 * can_handoff deny every pair rather than allow them.
 *
 * @param config Parsed engine config.
 * @req REQ-INFER-020
 * @version 2.0.2
 */
void ModelOrchestrator::build_routing_tables(const ParsedConfig& config) {
    for (const auto& [digit, tier_name] : config.routing.tier_map) {
        tier_map_[digit] = tier_name;
    }
    for (const auto& [src, targets] : config.routing.handoff_rules) {
        handoff_rules_[src] = std::unordered_set<std::string>(
            targets.begin(), targets.end());
    }
}

/**
 * @brief Load and activate the default inference tier.
 *
 * gh#157 (v2.13.0): routes through `get_model` — the same residency-gated
 * path a mid-session tier swap and a deferred first use take. Before this
 * it called `load_and_activate` directly, so the eager startup load skipped
 * the VRAM budget gate, fired no `Loaded` event and recorded no footprint:
 * the one load every consumer performs was the one the residency
 * bookkeeping could not see. Both paths are now identical after the load,
 * which is what makes `defer_load` a scheduling choice rather than a
 * different lifecycle.
 *
 * @param config Parsed engine config (unused — `config_` is already
 *        assigned by `initialize`, and `get_model` reads it).
 * @return true on success, false on a refused or failed activation.
 * @utility
 * @req REQ-INFER-019
 * @version 2.13.0
 */
bool ModelOrchestrator::activate_default_tier(const ParsedConfig& config) {
    // gh#157: `models.defer_load` leaves the default tier COLD until the
    // first caller needs it. Two idle hosts previously held two copies of
    // the same 4.8 GB GGUF in VRAM without ever being sent a run.
    const bool skip = config.models.defer_load
        || tiers_.find(default_tier_) == tiers_.end();
    if (skip) {
        if (config.models.defer_load) {
            logger->info("[residency] models.defer_load=true — default tier "
                         "'{}' loads on first use", default_tier_);
        }
        return true;
    }
    if (get_model(default_tier_) == nullptr) {
        logger->error("Failed to activate default tier: {}", default_tier_);
        return false;
    }
    logger->info("Activated default tier: {}", default_tier_);
    return true;
}

/**
 * @brief Load and activate the router model via SecondaryModelLoader.
 *
 * Delegates the load/unload lifecycle to `secondary_loader_` under the
 * `"router"` role (gh#27, v2.1.11). Preserves observable behavior:
 * router still loads at init when `models.router` is configured.
 *
 * @param config Parsed engine config.
 * @req REQ-INFER-020
 * @version 2.1.11
 */
void ModelOrchestrator::activate_router(const ParsedConfig& config) {
    if (!config.models.router) { return; }
    // Lifecycle now lives on SecondaryModelLoader (gh#27, v2.1.11).
    // Diagnostic-level logging is emitted by the loader itself.
    secondary_loader_.ensure_loaded("router", *config.models.router);
}

/**
 * @brief Activate the speculative-draft model when configured.
 *
 * Hands the consumer-supplied `ModelConfig` (from the YAML's
 * `inference.speculative.draft:` block) directly to the secondary
 * loader under the `"draft"` role. Speculative is opt-in — a load
 * failure here is logged and treated as "no draft available"
 * (degrades to plain decode) rather than blocking engine init.
 *
 * @param config Parsed engine config.
 * @req REQ-INFER-020
 * @version 2.9.0
 */
void ModelOrchestrator::activate_draft(const ParsedConfig& config) {
    const auto& spec = config.inference.speculative;
    if (!spec.enabled || spec.draft.path.empty()) { return; }
    // gh#106 (v2.9.0): under MTP the target owns the head (lazily, via
    // generate_mtp → setup_mtp_draft). Loading draft.path as a standalone
    // secondary backend here would double-load the head GGUF and never use
    // it — skip the gh#36 separate-draft activation entirely.
    if (spec.mtp) {
        logger->info("Speculative MTP: head '{}' is target-owned; skipping "
                     "separate draft activation", spec.draft.path.string());
        return;
    }
    // Full ModelConfig comes from the YAML's
    // `inference.speculative.draft:` block — every llama.cpp knob is
    // consumer-tunable. Defaults come from
    // `make_default_draft_model_config()` (gpu_layers=0,
    // flash_attn=false, context_length=8192, n_threads=4).
    secondary_loader_.ensure_loaded("draft", spec.draft);
}

/**
 * @brief Re-ensure router + draft after a release-all (gh#164). See header.
 * @dg_internal
 * @req REQ-INFER-020
 * @version 2.13.0
 */
void ModelOrchestrator::ensure_secondary_roles() {
    activate_router(config_);
    activate_draft(config_);
}

/**
 * @brief Initialize orchestrator: backends, routing, adapters, grammars.
 *
 * Adds speculative-draft activation alongside router activation in
 * v2.1.11 (gh#36) — the draft slot loads when `inference.speculative.
 * enabled` is true and a `draft_model` is configured. v2.2.4 (gh#57)
 * caches the VRAM budget from `ENTROPIC_VRAM_BUDGET_BYTES` so the
 * residency gate in `get_model` has a number to test against.
 *
 * @param config Parsed engine config.
 * @return true on success.
 * @utility
 * @version 2.13.0
 */
bool ModelOrchestrator::initialize(const ParsedConfig& config) {
    config_ = config;
    default_tier_ = config.models.default_tier;
    vram_budget_bytes_ = resolve_vram_budget_bytes();
    if (vram_budget_bytes_ > 0) {
        logger->info("[residency] VRAM budget: {} bytes "
                     "(ENTROPIC_VRAM_BUDGET_BYTES)",
                     vram_budget_bytes_);
    }

    // Route ggml/llama logs before any model loading.
    // gh#23 v2.3.24: `llama_log_path` overrides the hardcoded
    // `<log_dir>/llama_ggml.log` when non-empty. The non-empty-and-no-log-dir
    // case is also supported so consumers that want llama logs but
    // no session.log can opt in.
    if (config.ggml_logging) {
        std::string path;
        if (!config.llama_log_path.empty()) {
            path = config.llama_log_path.string();
        } else if (!config.log_dir.empty()) {
            path = (config.log_dir / "llama_ggml.log").string();
        }
        if (!path.empty()) {
            entropic_inference_log_to_file(path.c_str());
            logger->info("ggml logging: {}", path);
        }
    }

    logger->info("Initializing model orchestrator");

    if (!create_tier_backends(config)) { return false; }
    build_routing_tables(config);
    // gh#157 (v2.13.0): a no-op when `models.defer_load` is set.
    if (!activate_default_tier(config)) { return false; }
    activate_router(config);
    activate_draft(config);      // Speculative draft slot (v2.1.11)

    // LoRA adapter preload moved to first activation of the owning model
    // (gh#157): preloading here required the base model to already be
    // loaded, so it silently warned-and-skipped for every tier whose GGUF
    // was not the default one's — and `ensure_adapter_for_tier` then failed
    // with "not found or COLD" on the swap that needed it.
    load_bundled_grammars();     // Bundled grammars (v1.9.3)
    return true;
}

/**
 * @brief Shutdown — unload all models.
 *
 * Main-tier pool is unloaded directly; secondary roles (router, draft,
 * etc.) are released through `secondary_loader_.shutdown()` (v2.1.11).
 *
 * @req REQ-INFER-002
 * @req REQ-INFER-020
 * @version 2.1.11
 */
void ModelOrchestrator::shutdown() {
    logger->info("Shutting down model orchestrator");

    for (auto& [path, backend] : model_pool_) {
        if (backend->is_loaded()) {
            backend->unload();
        }
    }

    secondary_loader_.shutdown();
}

/**
 * @brief Orchestrate teardown order (gh#58 close-out). See header.
 *
 * Backends first (frees the llama_contexts), LoRA handles second — a LoRA
 * handle must outlive every context that referenced it, so the reverse
 * order is a use-after-free.
 *
 * @req REQ-INFER-002
 * @version 2.3.0
 */
ModelOrchestrator::~ModelOrchestrator() {
    // Order matters (gh#58 close-out, v2.3.0):
    //   1. Backends first → frees llama_contexts.
    //   2. LoRA adapter handles after → safe because the contexts
    //      that may have held HOT adapter references are gone.
    shutdown();
    lora_manager_.unload_all();
}

/**
 * @brief Resolve whether MTP should be attempted for this tier (gh#108,
 *        v2.9.4): `TierConfig::speculative_mtp` overrides the global
 *        `speculative.mtp` flag when set, else inherits it.
 * @param tier_name Tier whose override to consult.
 * @return The per-tier `speculative_mtp` value when the tier sets one —
 *         it wins over the global flag in both directions — else the global
 *         `inference.speculative.mtp`.
 * @req REQ-INFER-015
 * @version 2.9.4
 */
bool ModelOrchestrator::resolve_mtp_effective(const std::string& tier_name) const {
    auto it = config_.models.tiers.find(tier_name);
    if (it != config_.models.tiers.end() && it->second.speculative_mtp) {
        return *it->second.speculative_mtp;
    }
    return config_.inference.speculative.mtp;
}

/**
 * @brief Run a generate call through speculative (if enabled+pair
 *        compatible) or fall back to plain decode.
 * @dg_internal
 * @version 2.9.4
 */
GenerationResult ModelOrchestrator::run_generate_dispatch(
    InferenceBackend* model,
    const std::vector<Message>& messages,
    const GenerationParams& params,
    const std::string& tier_name) {
    GenerationResult result;
    bool kernel_ran = config_.inference.speculative.enabled
        && try_speculative_route(model, messages, params, tier_name, result);
    if (!kernel_ran) {
        result = model->generate(messages, params);
    }
    return result;
}

/**
 * @brief Route a generate call through the target-owned MTP kernel (gh#106).
 *
 * No separate draft backend + no compat-pair gate: the target backend owns
 * the MTP head (lazily set up against its live context). gh#108 (v2.9.1):
 * MTP **never silently falls back to plain decode** — when the consumer
 * enabled speculative.mtp, this route OWNS the outcome and ALWAYS returns
 * true. generate_mtp either runs or returns a loud typed error
 * (ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG for an out-of-envelope
 * request), which is propagated so the consumer corrects the config rather
 * than getting silent plain decode that masks MTP never engaging.
 *
 * @param model Active backend; a non-llama.cpp target yields NOT_SUPPORTED
 *        with "disable speculative.mtp" rather than a fallback.
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param on_token Per-token callback (may be empty).
 * @param cancel Cancel flag.
 * @param[out] result The MTP outcome — kernel result or typed loud error.
 * @return Always true: MTP owns the outcome, so the caller never falls
 *         through to plain decode.
 * @req REQ-INFER-015
 * @version 2.9.1
 */
bool ModelOrchestrator::try_mtp_route(
    InferenceBackend* model,
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view)> on_token,
    std::atomic<bool>& cancel,
    GenerationResult& result)
{
    auto* llama_target = dynamic_cast<LlamaCppBackend*>(model);
    if (llama_target == nullptr) {
        // Fail loud — no silent plain-decode fallback (gh#108).
        result = GenerationResult{};
        result.error_code = ENTROPIC_ERROR_NOT_SUPPORTED;
        result.error_message = "speculative.mtp enabled but the target backend "
                               "is not llama.cpp; disable speculative.mtp";
        result.finish_reason = "error";
        logger->error("{}", result.error_message);
    } else {
        result = llama_target->generate_mtp(
            messages, params, on_token, cancel,
            config_.inference.speculative.draft.path.string(),
            config_.inference.speculative.n_draft);
    }
    return true;  // MTP owns the outcome — never fall back to plain decode
}

/**
 * @brief gh#107: return true (and populate result) when draft looks like
 *        an MTP head GGUF routed to the classical separate-draft path.
 *
 * An MTP head GGUF (1-2 layers) crashes in fattn.cu when the classical
 * `generate_speculative_with_draft` path creates a separate KV context.
 * Fail loud with INCOMPATIBLE_CONFIG instead of crashing.
 *
 * @param draft  Draft LlamaCppBackend (model must be loaded for the check).
 * @param result [out] Populated on true return with
 *        SPECULATIVE_INCOMPATIBLE_CONFIG naming the layer count and the
 *        `speculative.mtp: true` knob.
 * @return True when the guard fires and result is populated; false otherwise.
 * @req REQ-INFER-015
 * @version 2.10.0
 */
static bool mtp_head_guard_fires(LlamaCppBackend* draft,
                                 GenerationResult& result) {
    auto* dm = draft->llama_model_ptr();
    if (dm == nullptr) { return false; }
    int n = llama_model_n_layer(dm);
    if (!looks_like_mtp_head(n)) { return false; }
    result.error_code = ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG;
    result.error_message = mtp_head_classical_path_error(n);
    result.finish_reason = "error";
    return true;
}

/**
 * @brief Common implementation: returns true if the speculative
 *        kernel ran (result populated), false to fall back to plain.
 *
 * Centralises the dynamic_cast + compat check shared by both
 * generate and generate_streaming (v2.1.11, gh#36). gh#106 (v2.9.0):
 * MTP routes out to try_mtp_route before the gh#36 compat path.
 *
 * gh#108 (v2.9.4): MTP-attempt was gated on `params.grammar.empty()` as a
 * request-level safety net. gh#108 (v2.10.0): that gate is removed —
 * `to_common_sampling` now propagates params.grammar to the MTP sampler
 * chain so grammar constraints are correctly enforced under speculative.mtp.
 * The per-tier `resolve_mtp_effective(tier_name)` override remains.
 *
 * gh#107 (v2.10.0): before entering the classical gh#36 draft path, check
 * whether the loaded draft GGUF looks like an MTP head (≤2 layers). If so,
 * fail loud with INCOMPATIBLE_CONFIG instead of crashing in fattn.cu.
 *
 * @dg_internal
 * @version 2.10.0 [reviewed]
 */
bool ModelOrchestrator::try_speculative_route_streaming(
    InferenceBackend* model,
    const std::vector<Message>& messages,
    const GenerationParams& params,
    const std::string& tier_name,
    std::function<void(std::string_view)> on_token,
    std::atomic<bool>& cancel,
    GenerationResult& result)
{
    // gh#106 (v2.9.0): MTP routes BEFORE the gh#36 compat/pair path — the
    // target owns the head (no separate draft backend), and MTP tolerates
    // shared-KV gemma4 archs the gh#36 compat gate rejects.
    if (resolve_mtp_effective(tier_name)) {  // gh#108 v2.10.0: grammar no longer blocks MTP
        return try_mtp_route(model, messages, params, on_token, cancel,
                             result);
    }
    auto compat = check_speculative_compat();
    bool kernel_ran = false;
    if (!compat.compatible) {
        logger->info("Speculative requested but pair incompatible "
                     "({}); using plain decode", compat.diagnostic);
    } else {
        auto* llama_target = dynamic_cast<LlamaCppBackend*>(model);
        auto* draft_be = secondary_loader_.get("draft");
        auto* llama_draft = dynamic_cast<LlamaCppBackend*>(draft_be);
        if (llama_target == nullptr || llama_draft == nullptr) {
            logger->info("Speculative compat passed but target/draft "
                         "is not llama.cpp; using plain decode");
        } else {
            // gh#107 (v2.10.0): guard against MTP head GGUFs on classical path.
            if (mtp_head_guard_fires(llama_draft, result)) {
                logger->error("{}", result.error_message);
                return true;
            }
            auto spec = llama_target->generate_speculative_with_draft(
                messages, params, on_token, cancel, *llama_draft,
                config_.inference.speculative.n_draft,
                config_.inference.speculative.draft.path.string());
            if (spec.error_code == ENTROPIC_ERROR_NOT_SUPPORTED) {
                logger->info("Speculative kernel returned NOT_SUPPORTED "
                             "({}); falling back", spec.error_message);
            } else {
                result = std::move(spec);
                kernel_ran = true;
            }
        }
    }
    return kernel_ran;
}

/**
 * @brief Non-streaming speculative route — wraps the streaming form
 *        with an empty on_token and a local cancel flag.
 *
 * gh#108: passes an EMPTY std::function (not a bound no-op lambda) so the MTP
 * path can distinguish non-streaming from streaming by callback bound-ness.
 * @dg_internal
 * @version 2.9.4
 */
bool ModelOrchestrator::try_speculative_route(
    InferenceBackend* model,
    const std::vector<Message>& messages,
    const GenerationParams& params,
    const std::string& tier_name,
    GenerationResult& result)
{
    std::atomic<bool> local_cancel{false};
    // gh#108: pass an EMPTY std::function (not a bound no-op lambda) so the MTP
    // path can distinguish non-streaming from streaming via the callback's
    // bound-ness. gh#36's emit guards `if (on_token)`, so empty is equivalent.
    return try_speculative_route_streaming(
        model, messages, params, tier_name,
        std::function<void(std::string_view)>{}, local_cancel, result);
}

// ── Generation ─────────────────────────────────────────────

/**
 * @brief Stage the turn's tool defs on the backend for common_chat (gh#87).
 *
 * Forwards `params.tools` (per-tier MCP JSON, empty for a tool-less turn) to
 * the backend so its render path emits the model's native tool-call format.
 * No-op for non-LlamaCpp backends. Called before every generate dispatch.
 *
 * @param model Active backend (may be null / non-LlamaCpp).
 * @param params Resolved generation params (carries `tools`).
 * @param require_tool_call Per-tier mandatory-tool flag staged alongside the
 *        defs; drives the render's tool_choice.
 * @req REQ-INFER-009
 * @version 2.10.4
 */
static void stage_active_tools(InferenceBackend* model,
                               const GenerationParams& params,
                               bool require_tool_call) {
    if (auto* llama = dynamic_cast<LlamaCppBackend*>(model)) {
        llama->set_active_tools(params.tools);
        llama->set_require_tool_call(require_tool_call);  // gh#134 (v2.10.4)
    }
}

/**
 * @brief Split tool calls out of a result (gh#87: common_chat or adapter).
 *
 * gh#87 Phase D: parse via common_chat (`parse_response`) ONLY when its
 * captured format is multi-parameter safe (`common_chat_parse_reliable` —
 * the dedicated PEG_GEMMA4 grammar). Autoparser families (Qwen, nemotron3)
 * fall back to their hand-rolled multi-parameter adapter, because the PEG
 * autoparser drops parameters past the first. Never both (double-parse
 * guard). raw_content always preserves the pre-split output.
 *
 * @param model Backend that produced the result (for common_chat routing).
 * @param adapter Tier adapter for the autoparser/fallback path (may be null).
 * @param[in,out] result Generation result (content split, tools set).
 * @req REQ-INFER-010
 * @version 2.10.3
 */
static void apply_adapter_parse(InferenceBackend* model,
                                ChatAdapter* adapter,
                                GenerationResult& result) {
    if (result.content.empty()) { return; }
    result.raw_content = result.content;
    // gh#108 (v2.10.3): one template-first/adapter-second rule, shared with
    // interface_factory. See response_parse.h for why content composes and
    // tool calls do not.
    auto parsed = parse_model_response(
        dynamic_cast<LlamaCppBackend*>(model), adapter, result.content);
    result.content = std::move(parsed.content);
    result.tool_calls = std::move(parsed.tool_calls);
}


/**
 * @brief Explain a turn that produced tokens but delivered no content (gh#137).
 *
 * The reasoning strip legitimately empties a generation that never closed its
 * reasoning block — surfacing raw reasoning as the answer would be worse. But
 * the operator has to know WHICH problem they have, and until v2.11.0 the
 * engine gave one message for both, telling them to raise max_tokens.
 *
 * gh#137 reported finish=stop with 154 chars delivering 0 chars. The model
 * ended that turn itself; no budget increase can help, and the advice sent the
 * reporter looking in the wrong place. This is the only site that can tell the
 * difference, because it is the only one holding finish_reason.
 *
 * gh#159: it is also the only site holding the PARSED TOOL CALLS, and it was
 * not passing them. `apply_adapter_parse` runs before this, moving tool calls
 * out of `content` — so a turn whose whole output was one tool call (the
 * normal shape under `tool_call_mode: sequential`) arrived here as content
 * empty / raw non-empty / finish "stop" and got gh#137's diagnosis verbatim,
 * telling operators of `enable_thinking: false` tiers that an unterminated
 * reasoning block was not converging. That fired on every tool-call turn of a
 * session. A parsed call is not a fault, so it is reported at INFO.
 *
 * @param result Completed generation result.
 * @req REQ-INFER-010
 * @version 2.13.0
 */
static void warn_if_content_vanished(const GenerationResult& result) {
    const auto cause = diagnose_empty_content(
        result.content.empty(), !result.raw_content.empty(),
        result.finish_reason, !result.tool_calls.empty());
    if (cause == EmptyContentCause::not_empty) { return; }
    if (cause == EmptyContentCause::tool_call_only) {
        logger->info("Turn delivered {} tool call(s) and no prose. {}",
                     result.tool_calls.size(), explain_empty_content(cause));
        return;
    }
    logger->warn("Turn produced {} raw chars but delivered no content. {}",
                 result.raw_content.size(), explain_empty_content(cause));
}

/**
 * @brief Diagnose a mandatory-tool turn that ran out of budget (gh#134).
 *
 * Under `tool_choice: REQUIRED` the gemma4 grammar is
 * `zero_or_more(any) + tool_call`: unbounded preamble, then a MANDATORY call.
 * So the grammar guarantees a call comes EVENTUALLY, not that it arrives
 * within max_tokens. Measured on E4B QAT: at 600 tokens the model could no
 * longer quit with prose (correct) but burned the budget narrating and was cut
 * off (`finish_reason == "length"`, zero calls); at 2000 it emitted a call on
 * all 10 turns.
 *
 * That failure is a budget misconfiguration, not a model or grammar fault, and
 * it is indistinguishable from a stall unless the engine says so — the same
 * diagnostic dead end gh#130 fixed on the bridge. `enable_thinking` is the
 * usual culprit because the thinking channel is what consumes the preamble.
 *
 * @param result Completed generation result.
 * @param tier_name Tier that produced it.
 * @param tiers Tier table, for the per-tier require_tool_call lookup.
 * @utility
 * @version 2.10.4
 */
static void warn_if_budget_starved_required_turn(
    const GenerationResult& result,
    const std::string& tier_name,
    const std::unordered_map<std::string, TierConfig>& tiers) {
    if (!result.tool_calls.empty()) { return; }
    auto it = tiers.find(tier_name);
    if (it == tiers.end()
        || !it->second.require_tool_call.value_or(false)) { return; }
    if (result.finish_reason != "length") { return; }
    logger->error(
        "Tier '{}' requires a tool call but hit its token budget before "
        "emitting one (finish_reason=length, 0 tool calls). The tool-call "
        "grammar permits unbounded text BEFORE the mandated call, so the call "
        "must fit inside max_tokens. Raise max_tokens, or disable "
        "enable_thinking on this tier — the thinking channel is what consumes "
        "the preamble. This is a budget misconfiguration, not a model failure.",
        tier_name);
}

/**
 * @brief Report every post-turn diagnostic from one call site (gh#137).
 *
 * Folded into a single entry point because the caller is `generate`, which sits
 * against the knots ABC ceiling — two adjacent warn calls pushed it over. Both
 * checks are no-ops on a healthy turn.
 *
 * @param result Completed generation result.
 * @param tier_name Selected tier.
 * @param tiers Configured tiers, for the require_tool_call budget check.
 * @req REQ-INFER-010
 * @version 2.11.0
 */
static void warn_turn_diagnostics(
    const GenerationResult& result,
    const std::string& tier_name,
    const std::unordered_map<std::string, TierConfig>& tiers) {
    warn_if_content_vanished(result);
    warn_if_budget_starved_required_turn(result, tier_name, tiers);
}

/**
 * @brief Resolve params + stage tools for a generate dispatch (gh#87).
 *
 * gh#105 (v2.8.3): the gh#103 sequential close-marker injection was REMOVED
 * from here — it ran PRE-render, off the previous/empty captured format, so it
 * never fired on the generation it was configured for (and never on the first
 * call). The backend now injects the marker POST-render
 * (LlamaCppBackend::effective_stop), using THIS call's resolved format.
 *
 * gh#154 (v2.13.0): grammar resolution moved to
 * `refuse_unresolved_tier_grammar`, called by every entry point BEFORE
 * `get_model`. An unregistered tier grammar has to refuse the run before a
 * model is resolved, and this function runs after one, so `params` reaches
 * here already grammar-resolved.
 *
 * @param model Active backend (tools staged here).
 * @param params Grammar-resolved generation params.
 * @param tier_name Selected tier.
 * @return Resolved params — per-tier sampler defaults applied — with the
 *         turn's tools and require_tool_call flag already staged on the
 *         backend.
 * @req REQ-INFER-009
 * @version 2.13.0
 */
GenerationParams ModelOrchestrator::resolve_and_stage(
    InferenceBackend* model,
    const GenerationParams& params,
    const std::string& tier_name) {
    GenerationParams resolved = params;
    apply_tier_sampler_defaults(resolved, tier_name);  // gh#82
    // gh#134 (v2.10.4): per-tier, never global — front-office tiers
    // legitimately answer in prose.
    bool require_tc = false;
    if (auto it = config_.models.tiers.find(tier_name);
        it != config_.models.tiers.end()) {
        require_tc = it->second.require_tool_call.value_or(false);
    }
    stage_active_tools(model, resolved, require_tc);   // gh#87 3b, gh#134
    return resolved;
}

/**
 * @brief Log the per-orchestration tier/adapter/timing summary.
 * @param result Generation result (carries timings).
 * @param selected Selected tier name.
 * @param adapter_name Adapter that ran.
 * @param params Resolved params (for grammar info).
 * @param routing_ms Routing time.
 * @param swap_ms Model-swap time.
 * @utility
 * @version 2.3.7
 */
static void log_orchestration(const GenerationResult& result,
                              const std::string& selected,
                              const std::string& adapter_name,
                              const GenerationParams& params,
                              double routing_ms, double swap_ms) {
    logger->info("Orchestration: tier={}, adapter={}, grammar={}",
                 selected, adapter_name,
                 params.grammar.empty() ? "unconstrained"
                                        : params.grammar_key);
    logger->info("Total: {:.0f}ms (route={:.0f}ms, swap={:.0f}ms, "
                 "gen={:.0f}ms)",
                 result.total_ms, routing_ms, swap_ms,
                 result.generation_time_ms);
}

/**
 * @brief Generate response using routed or explicit tier.
 *
 * Speculative routing added in v2.1.11 (gh#36): when the kernel is
 * configured and the target/draft pair is compatible, dispatches
 * through `LlamaCppBackend::generate_speculative_with_draft`; falls
 * back to plain decode otherwise. The dispatch decision is delegated
 * to `run_generate_dispatch` to keep this method under the SLOC gate.
 * v2.2.4 (gh#57): a refused activation now reports
 * `ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE` via `build_no_model_error`
 * instead of the generic `GENERATE_FAILED`.
 *
 * gh#154 (v2.13.0): the tier's grammar is resolved BEFORE the model is, so
 * a tier naming a grammar nobody registered returns
 * `ENTROPIC_ERROR_GRAMMAR_NOT_FOUND` without a swap, a prefill or a token.
 *
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param tier_name Explicit tier or empty for routing.
 * @return GenerationResult.
 * @dg_internal
 * @version 2.13.0 [reviewed]
 */
GenerationResult ModelOrchestrator::generate(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    const std::string& tier_name)
{
    auto t_start = now();

    // Route if no explicit tier
    std::string selected = tier_name;
    double routing_ms = 0.0;
    if (selected.empty()) {
        auto t_route = now();
        selected = route(messages);
        routing_ms = elapsed_ms(t_route, now());
    }

    // gh#154: an unregistered tier grammar refuses the run — before the
    // model is resolved, so a doomed run costs no swap and decodes nothing.
    GenerationParams resolved_params = params;
    auto refusal = refuse_unresolved_tier_grammar(resolved_params, selected);
    if (refusal.has_value()) { return *refusal; }

    // Get model (may trigger swap)
    auto t_swap = now();
    InferenceBackend* model = get_model(selected);
    double swap_ms = elapsed_ms(t_swap, now());

    if (!model) { return build_no_model_error(selected); }

    resolved_params =
        resolve_and_stage(model, resolved_params, selected);  // gh#87 3b

    // Generate — speculative routing applies here too (v2.1.11, gh#36)
    GenerationResult result = run_generate_dispatch(
        model, messages, resolved_params, selected);

    finish_generation(result, model, resolved_params, selected,
                      routing_ms, swap_ms, t_start);
    return result;
}

/**
 * @brief Batch generate with cancel — see header for contract.
 *
 * Bypasses `run_generate_dispatch` (speculative routing) because
 * speculative kernels live on the streaming path; batch only ever
 * calls plain decode. Calls `model->generate(messages, params,
 * cancel)` which polls cancel per token.
 *
 * gh#154 (v2.13.0): carries the same pre-model tier-grammar gate as the
 * other overload — one rule, every entry point.
 *
 * @dg_internal
 * @version 2.13.0 [reviewed]
 */
GenerationResult ModelOrchestrator::generate(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::atomic<bool>& cancel,
    const std::string& tier_name)
{
    auto t_start = now();

    std::string selected = tier_name;
    double routing_ms = 0.0;
    if (selected.empty()) {
        auto t_route = now();
        selected = route(messages);
        routing_ms = elapsed_ms(t_route, now());
    }

    // gh#154: same gate as the non-cancellable overload — one rule, every
    // entry point, always before a model is resolved.
    GenerationParams resolved_params = params;
    auto refusal = refuse_unresolved_tier_grammar(resolved_params, selected);
    if (refusal.has_value()) { return *refusal; }

    auto t_swap = now();
    InferenceBackend* model = get_model(selected);
    double swap_ms = elapsed_ms(t_swap, now());

    if (!model) { return build_no_model_error(selected); }

    resolved_params =
        resolve_and_stage(model, resolved_params, selected);  // gh#87 3b

    GenerationResult result = model->generate(
        messages, resolved_params, cancel);

    finish_generation(result, model, resolved_params, selected,
                      routing_ms, swap_ms, t_start);
    return result;
}

namespace {
/**
 * @brief The tier a batch arm runs under — `lead` when it names none.
 *
 * One spelling for a rule three loops in `generate_batch`'s neighbourhood
 * each used to inline, two of them indexing `tiers[i]` without checking
 * that `tiers` is as long as the request list.
 *
 * @param tiers Per-request tier names.
 * @param i Arm index.
 * @param lead Lead tier, used when the arm names none.
 * @return The arm's tier name.
 * @utility
 * @version 2.13.0
 */
const std::string& batch_arm_tier(const std::vector<std::string>& tiers,
                                  std::size_t i,
                                  const std::string& lead) {
    return (i < tiers.size() && !tiers[i].empty()) ? tiers[i] : lead;
}
}  // namespace

/**
 * @brief Same-prefix batch generation on a shared model — see header (gh#98).
 *
 * All requests resolve to ONE backend (the lead tier's model — tiers share the
 * pool). Each request's params are resolved per its own tier (grammar +
 * samplers), then the backend's `generate_batch` prefills the shared prefix
 * once and fans out. Tool staging is per-model, so this path targets
 * grammar-constrained requests (params.grammar), not common_chat tool
 * injection.
 *
 * gh#154 (v2.13.0): every arm's grammar resolves before the shared model
 * is touched, and one arm naming an unregistered grammar refuses the whole
 * batch — a single decode over a shared prefill cannot run
 * half-constrained.
 *
 * @dg_internal
 * @version 2.13.0 [reviewed]
 */
std::vector<GenerationResult> ModelOrchestrator::generate_batch(
    const std::vector<std::vector<Message>>& messages_list,
    const std::vector<GenerationParams>& params_list,
    const std::vector<std::string>& tiers,
    std::atomic<bool>& cancel)
{
    const std::size_t n = messages_list.size();
    const std::string lead =
        (tiers.empty() || tiers[0].empty()) ? "default" : tiers[0];

    // gh#154: every arm's grammar resolves before any model is touched. A
    // shared prefill cannot run half-constrained, so one unresolved arm
    // refuses the whole batch.
    std::vector<GenerationParams> resolved;
    auto refusal = refuse_unresolved_batch_grammars(
        params_list, tiers, lead, resolved);
    if (refusal.has_value()) {
        return std::vector<GenerationResult>(n, *refusal);
    }

    InferenceBackend* model = get_model(lead);
    if (model == nullptr) {
        return std::vector<GenerationResult>(n, build_no_model_error(lead));
    }
    stage_batch_arms(model, tiers, lead, resolved);

    auto results = model->generate_batch(messages_list, resolved, cancel);
    for (std::size_t i = 0; i < results.size(); ++i) {
        apply_adapter_parse(
            model, get_adapter(batch_arm_tier(tiers, i, lead)), results[i]);
        // gh#154: a batch request is a generation. Each arm carries its
        // OWN resolved params, so each gets its own provenance — a
        // per-batch record would hide a tier whose grammar missed.
        record_generation(results[i], resolved[i], model);
    }
    return results;
}

/**
 * @brief Apply tier sampler defaults + stage tools for every batch arm.
 *
 * Split out of `generate_batch` when the gh#154 grammar gate pushed it
 * over the ABC gate. Runs AFTER the model is resolved, which is exactly
 * why the grammar refusal could not live in here.
 *
 * @param model Backend all arms share.
 * @param tiers Per-request tier names ("" = `lead`).
 * @param lead Lead tier name.
 * @param resolved Grammar-resolved params, staged in place.
 * @dg_internal
 * @version 2.13.0
 */
void ModelOrchestrator::stage_batch_arms(
    InferenceBackend* model,
    const std::vector<std::string>& tiers,
    const std::string& lead,
    std::vector<GenerationParams>& resolved)
{
    for (std::size_t i = 0; i < resolved.size(); ++i) {
        resolved[i] = resolve_and_stage(
            model, resolved[i], batch_arm_tier(tiers, i, lead));
    }
}

/**
 * @brief Trampoline: bridges TokenCallback C signature to std::function.
 * @param data Token bytes.
 * @param len Byte count.
 * @param ud Pointer to `std::function<void(std::string_view)>`.
 * @utility
 * @version 2.10.0
 */
static void stream_token_trampoline(const char* data, std::size_t len,
                                    void* ud) {
    (*static_cast<std::function<void(std::string_view)>*>(ud))(
        std::string_view(data, len));
}

/**
 * @brief Streaming generation with speculative dispatch.
 *
 * Speculative routing added in v2.1.11 (gh#36): when the kernel is
 * configured and the target/draft pair is compatible, dispatches to
 * `LlamaCppBackend::generate_speculative_with_draft` via
 * `try_speculative_route_streaming`. Falls back to plain streaming on
 * NOT_SUPPORTED or compatibility failure, with a diagnostic logged.
 *
 * gh#108 (v2.10.0): `on_token` is wrapped with StreamThinkFilter so
 * thinking-channel tokens are stripped from the live stream. The
 * buffered `result.content` is post-processed via `apply_adapter_parse`
 * on return, mirroring the non-streaming generate() path. This also
 * enables MTP streaming (the streaming guard in mtp_unsupported_reason
 * is removed in the same gh#108 v2.10.0 change).
 *
 * gh#108 (v2.10.3): the filter's markers come from the resolved adapter, the
 * same source the buffered strip uses — v2.10.0 left it on a hardcoded
 * `<think>` pair that gemma4 never emits.
 *
 * gh#154 (v2.13.0): the tier-grammar gate runs before `get_model`, so an
 * unregistered tier grammar refuses the stream without emitting a token.
 *
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param on_token Per-token callback, wrapped by the reasoning filter.
 * @param cancel Cancel flag, polled by the backend once per token.
 * @param tier_name Explicit tier, or empty to route.
 * @return GenerationResult with content parsed by the shared rule; an
 *         ENTROPIC_ERROR_GENERATE_FAILED result when no model resolves for
 *         the tier, or ENTROPIC_ERROR_GRAMMAR_NOT_FOUND when its grammar
 *         is unregistered.
 * @req REQ-INFER-011
 * @req REQ-INFER-005
 * @req REQ-INFER-008
 * @req REQ-INFER-007
 * @version 2.13.0 [reviewed]
 */
GenerationResult ModelOrchestrator::generate_streaming(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view)> on_token,
    std::atomic<bool>& cancel,
    const std::string& tier_name)
{
    std::string selected = tier_name.empty() ? route(messages) : tier_name;

    // gh#154: refuse an unregistered tier grammar before the model is
    // resolved — no swap, no prefill, and not one token streamed.
    GenerationParams resolved_params = params;
    auto refusal = refuse_unresolved_tier_grammar(resolved_params, selected);
    if (refusal.has_value()) { return *refusal; }

    InferenceBackend* model = get_model(selected);

    if (!model) {
        GenerationResult err;
        err.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        err.error_message = "No model for tier: " + selected;
        err.finish_reason = "error";
        return err;
    }

    resolved_params =
        resolve_and_stage(model, resolved_params, selected);  // gh#87 3b

    // gh#108 (v2.10.3): strip this family's reasoning blocks from the live
    // stream. v2.10.0 added the filter but left it on its hardcoded `<think>`
    // pair, so gemma4's `<|channel>` passed straight through. Markers now come
    // from the resolved adapter, the same source the buffered strip uses.
    //
    // This is not cosmetic: ResponseGenerator::generate_streaming builds
    // result.content from its OWN token accumulator and discards what
    // apply_adapter_parse produced, so on the agent-loop streaming path this
    // filter is the only thing standing between raw reasoning and the
    // conversation history.
    auto* stream_adapter = get_adapter(selected);
    const auto markers = (stream_adapter != nullptr)
        ? stream_adapter->thinking_markers() : ThinkMarkers{};
    StreamThinkFilter filter(stream_token_trampoline, &on_token,
                             markers.open, markers.close);
    auto filtered = [&filter](std::string_view sv) {
        filter.on_token(sv.data(), sv.size());
    };

    GenerationResult result;
    bool routed = config_.inference.speculative.enabled
        && try_speculative_route_streaming(
               model, messages, resolved_params, selected, filtered, cancel,
               result);
    if (!routed) {
        result = model->generate_streaming(
            messages, resolved_params, filtered, cancel);
    }
    filter.flush();
    apply_adapter_parse(model, get_adapter(selected), result);
    // gh#134 (v2.10.4): name a budget-starved mandatory-tool turn.
    warn_turn_diagnostics(result, selected,
                                         config_.models.tiers);
    // gh#154: the streaming path runs the speculative/MTP kernels, so
    // leaving it unrecorded would omit exactly the decodes whose
    // drafted/accepted counts the record exists to carry.
    record_generation(result, resolved_params, model);
    return result;
}

// ── Routing ────────────────────────────────────────────────

/**
 * @brief Route to appropriate tier using router model.
 *
 * Guard updated in v2.1.11: routing requires `models.router` to be
 * configured (was: `router_` non-null). The slot is owned by
 * `secondary_loader_` since gh#27.
 *
 * Every decision is recorded in `last_routing_result_` — selected tier,
 * previous tier, raw model output, swap action — and pushed onto a tier
 * history bounded at 5 entries.
 *
 * @param messages Current conversation.
 * @return Selected tier name; the default tier when routing is disabled, no
 *         router is configured, or classification found no mapped digit.
 * @req REQ-INFER-020
 * @version 2.1.11
 */
std::string ModelOrchestrator::route(const std::vector<Message>& messages) {
    if (!config_.routing.enabled
        || !config_.models.router.has_value()) {
        logger->info("Route: routing disabled, using default '{}'",
                     default_tier_);
        last_routing_result_ = {default_tier_, "", "", "none", 0.0};
        return default_tier_;
    }

    auto [tier, raw] = classify_task(messages);
    last_routing_result_ = {tier, loaded_main_tier_, raw, "none", 0.0};

    // Track history
    tier_history_.push_back(tier);
    if (tier_history_.size() > 5) {
        tier_history_.erase(tier_history_.begin());
    }

    logger->info("[ROUTER] {} | raw='{}'", tier, raw);
    return tier;
}

/**
 * @brief Classify task using router model (raw completion).
 *
 * Fetches the router backend from `secondary_loader_.get("router")`
 * (v2.1.11). Returns an empty pair if the router slot has not been
 * loaded — the caller treats that as a routing miss and falls back to
 * the default tier.
 *
 * audit task #71 (v2.8.0): when `routing.classification_prompt` is configured,
 * prepend it to the router prompt so a general instruct model is actually told
 * the digit scheme (without it the router was fed a bare "<msg> ->" and just
 * continued the text — routing silently always fell back to the default tier).
 * The router token budget widens to 4 on that path to capture the digit after
 * a model's leading space; the bare-prompt path keeps the original 1-token
 * behavior for back-compat.
 *
 * @param messages Conversation history.
 * @return Pair of (tier_name, raw_digit): the mapped tier and the digit that
 *         selected it; ("","") when the router slot is not loaded; and
 *         (default_tier_, "") when the router emitted no mapped digit.
 * @req REQ-INFER-020
 * @version 2.8.1
 */
std::pair<std::string, std::string> ModelOrchestrator::classify_task(
    const std::vector<Message>& messages)
{
    std::string user_msg = extract_latest_user_message(messages);

    GenerationParams router_params;
    router_params.max_tokens = 1;
    router_params.temperature = 0.0f;

    auto* router_backend = secondary_loader_.get("router");
    if (router_backend == nullptr) {
        logger->warn("classify_task: router not loaded; returning empty");
        return {"", ""};
    }
    // audit task #71: a non-fine-tuned router fed the bare "<msg> ->" just
    // CONTINUES the text and never emits a routing digit, so classify_task
    // silently always fell back to the default tier. When the deployment
    // configures routing.classification_prompt, prepend it so a general
    // instruct model is actually told the digit scheme. (The trailing " ->"
    // still constrains it to a single digit, per build_classification_prompt.)
    std::string router_prompt = user_msg + " ->";
    const auto& cprompt = config_.routing.classification_prompt;
    if (cprompt.has_value() && !cprompt->empty()) {
        router_prompt = *cprompt + "\n" + user_msg + " ->";
        // A general instruct model emits a leading space before the digit;
        // max_tokens=1 would cut it off. 4 captures "<space>1"; the digit scan
        // below takes the first tier_map char. Only widened on the prompt path
        // so unconfigured deployments keep the original 1-token behavior.
        router_params.max_tokens = 4;
        // v2.8.1 (review #3): classification_prompt was parsed-but-never-read
        // before the v2.8.0 fix. Log when the active (prompt) path is taken so
        // a deployment carrying a stale prompt sees the inert->active switch +
        // the widened token budget instead of a silent behavior change.
        logger->info("classify_task: using configured classification_prompt "
                     "(router instructed; max_tokens widened to 4)");
    }
    auto result = router_backend->complete(router_prompt, router_params);
    std::string raw = result.content;

    // Trim whitespace
    auto start = raw.find_first_not_of(" \t\n\r");
    if (start != std::string::npos) {
        raw = raw.substr(start);
    }

    // Find matching tier
    for (char c : raw) {
        std::string digit(1, c);
        auto it = tier_map_.find(digit);
        if (it != tier_map_.end()) {
            logger->info("Route: digit='{}' -> tier='{}'",
                         digit, it->second);
            return {it->second, digit};
        }
    }

    logger->warn("Route: no valid digit in '{}', defaulting to {}",
                 raw, default_tier_);
    return {default_tier_, ""};
}

// ── Model access ───────────────────────────────────────────

/**
 * @brief Get model for tier, loading/swapping as needed.
 * @param tier_name Tier name.
 * @return Backend pointer, or nullptr if unavailable.
 * @dg_internal
 * @version 1.9.2
 */
/**
 * @brief Reuse-hit bookkeeping for `get_model`.
 *
 * Records the activation timestamp for LRU tracking and fires an
 * ActivationSwap residency event when the active tier changed (i.e.
 * multi-resident hit: the new tier was already loaded). Same-tier
 * reuse simply refreshes the timestamp.
 *
 * @dg_internal
 * @version 2.2.4
 */
void ModelOrchestrator::record_activation_reuse(
    const std::string& tier_name) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_).count();
    bool tier_changed = (loaded_main_tier_ != tier_name);
    tier_last_activation_ms_[tier_name] = now_ms;
    if (!tier_changed) { return; }
    auto tier_it = config_.models.tiers.find(tier_name);
    std::string path = tier_it != config_.models.tiers.end()
        ? tier_it->second.path.string() : "";
    size_t footprint = tier_footprint_bytes_.count(tier_name)
        ? tier_footprint_bytes_[tier_name]
        : estimate_footprint_bytes(tier_name);
    tier_footprint_bytes_[tier_name] = footprint;
    loaded_main_tier_ = tier_name;
    fire_residency_observer(ResidencyEvent::ActivationSwap,
                            tier_name, path, footprint);
}

// Defined lower down, beside estimate_footprint_bytes which shares it.
static FootprintInputs footprint_inputs_for(
    const TierConfig& tier_cfg, uint64_t weights_bytes, int vram_reserve_mb);

/**
 * @brief Log what WOULD fit when a tier is refused for VRAM (gh#142).
 *
 * Turns a refusal into an actionable setting. Reports the largest context
 * length that fits the measured budget, or states that the context length is
 * not the lever when nothing fits at any context.
 *
 * @param tier_name Tier being refused.
 * @dg_internal
 * @req REQ-INFER-019
 * @version 2.11.0
 */
void ModelOrchestrator::log_fit_recommendation(
    const std::string& tier_name) const {
    auto tier_it = config_.models.tiers.find(tier_name);
    if (tier_it == config_.models.tiers.end()) { return; }
    const auto& tier_cfg = tier_it->second;
    std::error_code ec;
    auto weights = std::filesystem::file_size(tier_cfg.path, ec);
    if (ec) { return; }
    FootprintInputs in = footprint_inputs_for(
        tier_cfg, weights, config_.vram_reserve_mb);
    int fits = recommend_context_length(in, vram_budget_bytes_);
    if (fits > 0) {
        logger->error("[residency] tier '{}' fits at context_length={} "
                      "(requested {}) — lower it, or reduce gpu_layers to "
                      "keep the requested context",
                      tier_name, fits, tier_cfg.context_length);
    } else {
        logger->error("[residency] tier '{}' does not fit at ANY context "
                      "length: weights{} alone exceed the {} MiB budget. "
                      "Reduce gpu_layers, or use a smaller quantization.",
                      tier_name,
                      in.mmproj_bytes > 0 ? " + vision projector" : "",
                      vram_budget_bytes_ / (1024 * 1024));
    }
}

/**
 * @brief VRAM-budget admission test (gh#57).
 *
 * Estimates the tier's footprint, memoizes it, and rejects with
 * `last_residency_error_ = TIER_MODEL_TOO_LARGE` when the single-tier
 * estimate exceeds a known engine VRAM budget. Returns true to admit.
 *
 * @dg_internal
 * @version 2.11.0
 */
bool ModelOrchestrator::residency_admits(const std::string& tier_name) {
    size_t footprint = estimate_footprint_bytes(tier_name);
    if (footprint > 0) {
        tier_footprint_bytes_[tier_name] = footprint;
    }
    if (vram_budget_bytes_ > 0 && footprint > vram_budget_bytes_) {
        logger->error("[residency] tier '{}' footprint {} bytes "
                      "exceeds VRAM budget {} bytes — "
                      "TIER_MODEL_TOO_LARGE (gh#57)",
                      tier_name, footprint, vram_budget_bytes_);
        // gh#142: a bare refusal leaves the operator with nothing to act on.
        // Say what WOULD fit, so the answer is a setting they can apply rather
        // than a wall. 0 means even an empty context does not fit, in which
        // case the context length is not the lever and saying so is honest.
        log_fit_recommendation(tier_name);
        last_residency_error_ = ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE;
        return false;
    }
    return true;
}

/**
 * @brief Cold-path tier activation with residency bookkeeping.
 *
 * Drives `load_and_activate` on the backend; on success records the
 * activation timestamp and fires a `Loaded` residency event.
 *
 * @dg_internal
 * @version 2.2.4
 */
/**
 * @brief Build a typed GenerationResult for a get_model() failure.
 *
 * Threads the orchestrator's `last_residency_error_` stash into the
 * result so the facade surfaces `TIER_MODEL_TOO_LARGE` distinctly from
 * generic `GENERATE_FAILED`. Always clears the stash.
 *
 * @dg_internal
 * @version 2.2.4
 */
GenerationResult ModelOrchestrator::build_no_model_error(
    const std::string& tier_name) {
    GenerationResult err;
    err.finish_reason = "error";
    if (last_residency_error_ != ENTROPIC_OK) {
        err.error_code = last_residency_error_;
        err.error_message = "Tier '" + tier_name + "' model exceeds the "
                            "engine's VRAM budget (gh#57)";
        last_residency_error_ = ENTROPIC_OK;
    } else {
        err.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        err.error_message = "No model available for tier: " + tier_name;
    }
    return err;
}

/**
 * @brief Cold-path tier activation with residency bookkeeping.
 *
 * Drives `load_and_activate` on the backend; on success records the
 * activation timestamp and fires a `Loaded` residency event. Returns
 * the activated backend pointer, or nullptr on activation failure.
 *
 * @param tier_name Tier name (must be in `config_.models.tiers`).
 * @param backend   Backend shared with the tier_map entry.
 * @return Activated backend, or nullptr.
 * @dg_internal
 * @version 2.13.0
 */
InferenceBackend* ModelOrchestrator::activate_and_track(
    const std::string& tier_name,
    const std::shared_ptr<InferenceBackend>& backend) {
    auto tier_it = config_.models.tiers.find(tier_name);
    bool activated = tier_it != config_.models.tiers.end()
        && backend->load_and_activate(tier_it->second);
    if (!activated) {
        logger->error("Failed to activate tier: {}", tier_name);
        return nullptr;
    }
    loaded_main_tier_ = tier_name;
    last_routing_result_.swap_action = "loaded";
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_).count();
    tier_last_activation_ms_[tier_name] = now_ms;
    size_t footprint = tier_footprint_bytes_.count(tier_name)
        ? tier_footprint_bytes_[tier_name] : 0;
    // gh#157: the model exists now, so its adapters can bind. This is the
    // only moment at which that is true for a non-default tier.
    preload_adapters_for_model(backend.get());
    fire_residency_observer(ResidencyEvent::Loaded,
                            tier_name, tier_it->second.path.string(),
                            footprint);
    return backend.get();
}

/**
 * @brief Get the backend for a tier, loading/swapping/admitting as needed.
 *
 * Resolves the tier name (falling back to `routing.fallback_tier` when
 * unknown), reuses a hot backend or admits + activates a cold one
 * through the residency gate. Returns nullptr on a refused activation
 * (with `last_residency_error_` set) or on a real load failure.
 *
 * @param tier_name Requested tier name.
 * @return Backend pointer, or nullptr.
 * @dg_internal
 * @version 2.13.0
 */
InferenceBackend* ModelOrchestrator::get_model(const std::string& tier_name) {
    std::lock_guard<std::mutex> lock(swap_mutex_);

    auto it = tiers_.find(tier_name);
    std::string effective_tier = tier_name;
    if (it == tiers_.end()) {
        it = tiers_.find(config_.routing.fallback_tier);
        if (it != tiers_.end()) {
            effective_tier = config_.routing.fallback_tier;
        }
    }

    InferenceBackend* result = nullptr;
    if (it != tiers_.end() && it->second->is_active()) {
        last_routing_result_.swap_action = "reused";
        record_activation_reuse(effective_tier);
        result = it->second.get();
    } else if (it != tiers_.end() && residency_admits(effective_tier)) {
        deactivate_current_if_needed(it->second.get());
        result = activate_and_track(effective_tier, it->second);
    }

    // Ensure correct LoRA adapter for this tier (v1.9.2)
    if (result) {
        ensure_tier_lora(tier_name, result);
        // gh#164: `entropic_release_model(NULL)` drops the router and draft
        // too, and neither reloads itself — routing would degrade to the
        // default tier silently and permanently. No-op when they are loaded.
        ensure_secondary_roles();
    }

    return result;
}

/**
 * @brief Ensure the active tier's LoRA adapter is loaded.
 * @param tier_name Tier whose adapter to ensure.
 * @param result Active backend.
 * @dg_internal
 * @version 2.3.7
 */
void ModelOrchestrator::ensure_tier_lora(const std::string& tier_name,
                                         InferenceBackend* result) {
    auto* llama_backend = dynamic_cast<LlamaCppBackend*>(result);
    llama_context* ctx = llama_backend
        ? llama_backend->llama_context_ptr() : nullptr;
    double adapter_ms = ensure_adapter_for_tier(tier_name, ctx);
    last_routing_result_.adapter_swap_ms = adapter_ms;
    last_routing_result_.adapter_name = lora_manager_.active_adapter();
}

/**
 * @brief Deactivate current main tier for swap.
 *
 * keep_warm=true → WARM. keep_warm=false → COLD. v2.2.4 (gh#57): the
 * COLD-path unload fires a `ResidencyEvent::Evicted` so the residency
 * observer sees every "tier model just left VRAM" transition.
 *
 * @param incoming The backend about to be activated.
 * @dg_internal
 * @version 2.3.7
 */
void ModelOrchestrator::deactivate_current_if_needed(InferenceBackend* incoming) {
    auto it = loaded_main_tier_.empty()
        ? tiers_.end() : tiers_.find(loaded_main_tier_);

    bool should_swap = it != tiers_.end()
        && it->second.get() != incoming
        && it->second->is_loaded();

    if (!should_swap) {
        return;
    }

    // Cascade: unload adapters for this base model (v1.9.2)
    auto* llama_backend = dynamic_cast<LlamaCppBackend*>(it->second.get());
    if (llama_backend) {
        lora_manager_.unload_all_for_model(
            llama_backend->llama_model_ptr(),
            llama_backend->llama_context_ptr());
    }

    unload_or_warm_current(it->second.get());
}

/**
 * @brief Warm-deactivate or cold-unload the current main tier.
 * @param current The backend leaving the active slot.
 * @dg_internal
 * @version 2.3.7
 */
void ModelOrchestrator::unload_or_warm_current(InferenceBackend* current) {
    auto cfg_it = config_.models.tiers.find(loaded_main_tier_);
    bool keep_warm = cfg_it != config_.models.tiers.end()
        && cfg_it->second.keep_warm;

    if (keep_warm) {
        logger->info("Deactivating {} (keep_warm=true)", loaded_main_tier_);
        current->deactivate();
        return;
    }
    logger->info("Unloading {} (keep_warm=false)", loaded_main_tier_);
    std::string path = cfg_it != config_.models.tiers.end()
        ? cfg_it->second.path.string() : "";
    size_t footprint = tier_footprint_bytes_.count(loaded_main_tier_)
        ? tier_footprint_bytes_[loaded_main_tier_] : 0;
    std::string evicted_tier = loaded_main_tier_;
    current->unload();
    fire_residency_observer(ResidencyEvent::Evicted,
                            evicted_tier, path, footprint);
}

// ── Queries ────────────────────────────────────────────────

/**
 * @brief Last routing result.
 * @return The RoutingResult recorded by the most recent route() — selected
 *         tier, previous tier, raw router output, swap action and timings.
 *         Default-constructed before the first route.
 * @req REQ-INFER-020
 * @version 1.8.2
 */
RoutingResult ModelOrchestrator::last_routing_result() const {
    return last_routing_result_;
}

/**
 * @brief Last used tier name.
 * @dg_internal
 * @version 1.8.2
 */
std::string ModelOrchestrator::last_used_tier() const {
    return loaded_main_tier_;
}

/**
 * @brief Currently loaded model tier names.
 *
 * Includes `"router"` when the secondary loader reports the role as
 * loaded (v2.1.11, gh#27 — previously checked the raw `router_` field).
 *
 * @return Tier names whose backend reports is_loaded(), plus `"router"` when
 *         the secondary loader holds that role; a role whose load failed is
 *         absent.
 * @req REQ-INFER-020
 * @version 2.1.11
 */
std::vector<std::string> ModelOrchestrator::loaded_models() const {
    std::vector<std::string> result;
    for (const auto& [name, backend] : tiers_) {
        if (backend->is_loaded()) {
            result.push_back(name);
        }
    }
    if (secondary_loader_.is_loaded("router")) {
        result.push_back("router");
    }
    return result;
}

/**
 * @brief All configured tier names.
 * @dg_internal
 * @version 2.1.11
 */
std::vector<std::string> ModelOrchestrator::available_models() const {
    std::vector<std::string> result;
    for (const auto& [name, _] : tiers_) {
        result.push_back(name);
    }
    if (config_.models.router.has_value()) {
        result.push_back("router");
    }
    return result;
}

/**
 * @brief Get the inference backend for a tier.
 * @param tier_name Tier name.
 * @return Backend pointer, or nullptr if not found.
 * @utility
 * @version 1.10.2
 */
InferenceBackend* ModelOrchestrator::get_backend(
    const std::string& tier_name) const {
    auto it = tiers_.find(tier_name);
    if (it == tiers_.end()) { return nullptr; }
    return it->second.get();
}

/**
 * @brief Public residency-gated activation (gh#157). See header.
 * @param tier_name Tier to make resident.
 * @return ACTIVE backend, or nullptr.
 * @utility
 * @req REQ-INFER-019
 * @version 2.13.0
 */
InferenceBackend* ModelOrchestrator::ensure_model(
    const std::string& tier_name) {
    return get_model(tier_name);
}

/**
 * @brief Release resident model(s) (gh#164). See header.
 * @param tier_name Tier to release, or empty for all.
 * @return ENTROPIC_OK, or MODEL_NOT_FOUND for an unknown tier.
 * @utility
 * @req REQ-INFER-019
 * @version 2.13.0
 */
entropic_error_t ModelOrchestrator::release_models(
    const std::string& tier_name) {
    std::lock_guard<std::mutex> lock(swap_mutex_);

    if (!tier_name.empty()) {
        auto it = tiers_.find(tier_name);
        if (it == tiers_.end()) {
            logger->error("[residency] release: unknown tier '{}'",
                          tier_name);
            return ENTROPIC_ERROR_MODEL_NOT_FOUND;
        }
        release_backend(it->second.get());
        return ENTROPIC_OK;
    }

    std::unordered_set<const InferenceBackend*> seen;
    for (const auto& [name, backend] : tiers_) {
        (void)name;
        if (backend && seen.insert(backend.get()).second) {
            release_backend(backend.get());
        }
    }
    // Secondary roles (router, speculative draft). They re-ensure on the
    // next cold activation — see `activate_and_track`.
    secondary_loader_.shutdown();
    return ENTROPIC_OK;
}

/**
 * @brief Unload one backend, keeping its adapter registrations (gh#164).
 *
 * Adapter handles are freed BEFORE the model they were initialised
 * against — the reverse order is a use-after-free, the same ordering
 * `~ModelOrchestrator` documents.
 *
 * @param backend Backend to release. Null or already-COLD is a no-op.
 * @dg_internal
 * @req REQ-INFER-019
 * @version 2.13.0
 */
void ModelOrchestrator::release_backend(InferenceBackend* backend) {
    if (backend == nullptr || !backend->is_loaded()) { return; }

    auto* llama_backend = dynamic_cast<LlamaCppBackend*>(backend);
    if (llama_backend != nullptr) {
        // KEEPS the registrations, unlike the swap path's
        // `unload_all_for_model` — see AdapterManager (gh#164).
        lora_manager_.release_handles_for_model(
            llama_backend->llama_model_ptr(),
            llama_backend->llama_context_ptr());
    }
    backend->unload();
    announce_eviction(backend);
}

/**
 * @brief Fire Evicted for every tier that was backed by `backend` (gh#164).
 *
 * Every one of them was reported resident by `residency_snapshot_json`
 * (which keys on the backend's loaded state), so every one of them has
 * just stopped being resident and the observer is told about each.
 *
 * @param backend Backend that was just unloaded.
 * @dg_internal
 * @req REQ-INFER-019
 * @version 2.13.0
 */
void ModelOrchestrator::announce_eviction(const InferenceBackend* backend) {
    for (const auto& [name, bound] : tiers_) {
        if (bound.get() != backend) { continue; }
        auto cfg_it = config_.models.tiers.find(name);
        std::string path = cfg_it != config_.models.tiers.end()
            ? cfg_it->second.path.string() : "";
        auto fp_it = tier_footprint_bytes_.find(name);
        size_t footprint = fp_it != tier_footprint_bytes_.end()
            ? fp_it->second : 0;
        // Leave no stale incumbent: the next activation reads this to
        // decide whether a swap-out is needed.
        if (loaded_main_tier_ == name) { loaded_main_tier_.clear(); }
        fire_residency_observer(ResidencyEvent::Evicted, name, path,
                                footprint);
    }
}

/**
 * @brief Config-only vision capability for a tier (gh#157). See header.
 * @param tier_name Tier name.
 * @return true when the tier declares "vision" or carries an mmproj path.
 * @utility
 * @req REQ-INFER-025
 * @version 2.13.0
 */
bool ModelOrchestrator::tier_declares_vision(
    const std::string& tier_name) const {
    auto it = config_.models.tiers.find(tier_name);
    if (it == config_.models.tiers.end()) { return false; }
    return it->second.has_capability("vision")
        || !it->second.mmproj_path.empty();
}

/**
 * @brief Check if handoff is permitted.
 * @param from Source tier name.
 * @param to Candidate destination tier name.
 * @return true only when `from` has an explicit rule set listing `to`; an
 *         empty rule set denies every pair.
 * @req REQ-INFER-020
 * @version 1.8.2
 */
bool ModelOrchestrator::can_handoff(
    const std::string& from, const std::string& to) const
{
    auto it = handoff_rules_.find(from);
    if (it == handoff_rules_.end()) {
        return false;
    }
    return it->second.count(to) > 0;
}

/**
 * @brief Get adapter for a tier.
 * @dg_internal
 * @version 1.8.2
 */
ChatAdapter* ModelOrchestrator::get_adapter(const std::string& tier_name) const {
    auto it = adapters_.find(tier_name);
    if (it != adapters_.end()) {
        return it->second.get();
    }
    return nullptr;
}

// ── LoRA adapter management (v1.9.2) ──────────────────────

/**
 * @brief Ensure the correct LoRA adapter is active for a tier.
 *
 * If the tier has adapter_path configured, swaps to that adapter.
 * If the tier has no adapter, deactivates any active adapter.
 * Clears KV cache after swap (stale entries from prior adapter).
 *
 * @param tier_name Target tier.
 * @param ctx llama_context for activation.
 * @return Adapter swap time in milliseconds.
 * @dg_internal
 * @version 1.9.2
 */
/**
 * @brief Deactivate any active LoRA adapter.
 * @param ctx llama_context to clear from.
 * @return true if an adapter was deactivated.
 * @dg_internal
 * @version 1.9.2
 */
bool ModelOrchestrator::deactivate_if_active(llama_context* ctx) {
    if (lora_manager_.active_adapter().empty()) {
        return false;
    }
    lora_manager_.deactivate(ctx);
    return true;
}

/**
 * @brief Ensure correct LoRA adapter is active for a tier.
 * @param tier_name Target tier.
 * @param ctx llama_context for activation.
 * @return Adapter swap time in milliseconds.
 * @dg_internal
 * @version 1.9.2
 */
double ModelOrchestrator::ensure_adapter_for_tier(
    const std::string& tier_name, llama_context* ctx)
{
    auto tier_it = config_.models.tiers.find(tier_name);
    if (tier_it == config_.models.tiers.end()) {
        return 0.0;
    }

    const auto& tier_cfg = tier_it->second;
    auto t_start = now();
    bool needs_kv_clear = false;

    if (!tier_cfg.adapter_path) {
        needs_kv_clear = deactivate_if_active(ctx);
    } else if (lora_manager_.active_adapter() != tier_name) {
        needs_kv_clear = lora_manager_.swap(tier_name, ctx);
        if (!needs_kv_clear) {
            logger->warn("Adapter swap to '{}' failed", tier_name);
        }
    }

    if (needs_kv_clear && ctx) {
        llama_memory_clear(llama_get_memory(ctx), true);
        logger->info("Adapter swap for tier '{}' in {:.1f}ms",
                    tier_name, elapsed_ms(t_start, now()));
    }

    return elapsed_ms(t_start, now());
}

/**
 * @brief Preload the LoRA adapters of every tier backed by one model.
 *
 * Runs at ACTIVATION of that model (gh#157), not at engine init. The
 * init-time version required the base model to already be loaded, which is
 * true of exactly one tier — the default one — so every tier on a different
 * GGUF logged "model not loaded", skipped, and was never retried. The swap
 * that later needed the adapter then failed with "not found or COLD". With
 * `models.defer_load` the init-time version would have skipped ALL of them.
 *
 * Adapters already WARM/HOT are left alone, so re-activating a model does
 * not re-init or duplicate them. A registration whose handle was released
 * (gh#164) reads COLD and is re-bound here against the reloaded model.
 *
 * @param backend Freshly activated backend.
 * @dg_internal
 * @req REQ-INFER-023
 * @version 2.13.0
 */
void ModelOrchestrator::preload_adapters_for_model(
    InferenceBackend* backend) {
    auto* llama_backend = dynamic_cast<LlamaCppBackend*>(backend);
    if (!llama_backend || !llama_backend->llama_model_ptr()) { return; }

    int loaded = 0;
    for (const auto& [name, tier_cfg] : config_.models.tiers) {
        auto tier_it = tiers_.find(name);
        bool mine = tier_cfg.adapter_path.has_value()
            && tier_it != tiers_.end()
            && tier_it->second.get() == backend
            && lora_manager_.state(name) == AdapterState::COLD;
        if (mine && lora_manager_.load(name, *tier_cfg.adapter_path,
                                       llama_backend->llama_model_ptr(),
                                       tier_cfg.adapter_scale)) {
            ++loaded;
        }
    }

    if (loaded > 0) {
        logger->info("Preloaded {} LoRA adapter(s) to WARM", loaded);
    }
}

// ── Grammar registry (v1.9.3) ──────────────────────────────

/**
 * @brief Load bundled grammars from data directory.
 *
 * Scans ENTROPIC_DATA_DIR/grammars/ for .gbnf files and registers
 * each with the grammar registry.
 *
 * @dg_internal
 * @version 2.0.6
 */
void ModelOrchestrator::load_bundled_grammars() {
    std::filesystem::path grammar_dir;
    if (!config_.config_dir.empty()) {
        grammar_dir = config_.config_dir / "grammars";
    }
    if (grammar_dir.empty() || !std::filesystem::is_directory(grammar_dir)) {
        // Fallback set by facade via load_grammars_from() if config_dir
        // doesn't have a grammars subdir. Check if already loaded.
        logger->info("No bundled grammar directory found, skipping");
        return;
    }

    size_t count = grammar_registry_.load_bundled(grammar_dir);
    logger->info("Grammar registry: {} grammar(s) loaded from {}",
                 count, grammar_dir.string());
}

/**
 * @brief Load grammars from an explicit directory path.
 *
 * Called by the facade after data-dir resolution. This is the
 * fallback path when config_dir doesn't contain a grammars subdir
 * (e.g., installed layout where grammars live under share/entropic).
 *
 * @param grammar_dir Path to directory containing .gbnf files.
 * @return Number of grammars loaded.
 * @dg_internal
 * @version 2.0.6
 */
size_t ModelOrchestrator::load_grammars_from(
    const std::filesystem::path& grammar_dir) {
    if (!std::filesystem::is_directory(grammar_dir)) {
        return 0;
    }
    auto count = grammar_registry_.load_bundled(grammar_dir);
    logger->info("Grammar registry: {} grammar(s) loaded from {}",
                 count, grammar_dir.string());
    return count;
}

/**
 * @brief Invalidate prompt caches across every pooled backend.
 *
 * Called on identity content changes so no cached prefix is served
 * against the new system prompt. (P1-7, 2.0.6-rc16). Fans out to
 * secondary roles (router, draft) via SecondaryModelLoader (v2.1.11).
 *
 * @req REQ-INFER-020
 * @version 2.1.11
 */
void ModelOrchestrator::clear_all_prompt_caches() {
    for (auto& [_, backend] : model_pool_) {
        if (backend) { backend->clear_prompt_cache(); }
    }
    secondary_loader_.clear_all_prompt_caches();
    logger->info("Prompt caches invalidated across all backends "
                 "(identity change)");
}

/**
 * @brief Vision-capability lookup (gh#41, v2.1.8).
 * @return true if any configured tier declares "vision"; false lets the
 *         facade short-circuit with ENTROPIC_ERROR_NO_VISION_TIER instead
 *         of dispatching a turn no tier can handle.
 * @req REQ-INFER-025
 * @version 2.1.8
 */
bool ModelOrchestrator::has_vision_capable_tier() const {
    for (const auto& [_, tier] : config_.models.tiers) {
        if (tier.has_capability("vision")) { return true; }
    }
    return false;
}

/**
 * @brief First vision-capable tier name (gh#41, v2.1.8).
 * @return The canonical vision tier's name, or "" when no configured tier
 *         declares the capability.
 * @req REQ-INFER-025
 * @version 2.1.8
 */
std::string ModelOrchestrator::select_vision_tier() const {
    for (const auto& [name, tier] : config_.models.tiers) {
        if (tier.has_capability("vision")) { return name; }
    }
    return "";
}

/**
 * @brief Resolve the active main-tier llama_model* for compat lookup.
 *
 * @return Pointer to the loaded llama_model, or nullptr when no main
 *         tier is loaded or the backend is not LlamaCppBackend.
 * @dg_internal
 * @version 2.1.11
 */
static llama_model* resolve_target_model(
    const std::shared_ptr<InferenceBackend>& tier_backend) {
    if (!tier_backend || !tier_backend->is_loaded()) {
        return nullptr;
    }
    auto* llama_be = dynamic_cast<LlamaCppBackend*>(tier_backend.get());
    return (llama_be == nullptr) ? nullptr : llama_be->llama_model_ptr();
}

/**
 * @brief Resolve target+draft llama_model pointers from current state.
 *
 * @param[out] target_out Filled with the active main tier's llama_model.
 * @param[out] draft_out  Filled with the configured draft's llama_model.
 * @return Empty string on success; otherwise a diagnostic identifying
 *         which side is missing.
 * @dg_internal
 * @version 2.1.11
 */
std::string ModelOrchestrator::resolve_speculative_pair(
    llama_model*& target_out, llama_model*& draft_out) const {
    target_out = nullptr;
    draft_out = nullptr;
    std::string err;

    auto tier_it = tiers_.find(loaded_main_tier_);
    if (tier_it == tiers_.end()) {
        err = "no main tier loaded";
    } else {
        target_out = resolve_target_model(tier_it->second);
        if (target_out == nullptr) {
            err = "main tier backend is not a llama.cpp backend or "
                  "is not loaded";
        } else {
            auto* draft_backend = secondary_loader_.get("draft");
            if (draft_backend == nullptr || !draft_backend->is_loaded()) {
                err = "no draft model configured for speculative "
                      "decoding "
                      "(set inference.speculative.draft_model)";
            } else {
                auto* d = dynamic_cast<LlamaCppBackend*>(draft_backend);
                draft_out = (d == nullptr) ? nullptr : d->llama_model_ptr();
                if (draft_out == nullptr) {
                    err = "draft backend is not a llama.cpp backend";
                }
            }
        }
    }
    return err;
}

/**
 * @brief Speculative compatibility check (target vs draft).
 *
 * Reads the active main tier as the target and the `"draft"` slot on
 * the secondary loader as the draft. Returns a structured diagnostic
 * the C ABI can forward to consumers.
 *
 * @return SpeculativeCompatInfo with compatible flag + diagnostic.
 * @dg_internal
 * @version 2.1.11
 */
ModelOrchestrator::SpeculativeCompatInfo
ModelOrchestrator::check_speculative_compat() const {
    SpeculativeCompatInfo info;
    llama_model* target_model = nullptr;
    llama_model* draft_model = nullptr;
    info.diagnostic = resolve_speculative_pair(target_model, draft_model);
    if (info.diagnostic.empty()) {
        auto result = entropic::speculative::check_compat(
            target_model, draft_model);
        info.compatible = result.compatible;
        info.diagnostic = std::move(result.diagnostic);
    }
    return info;
}

/**
 * @brief Shared tail of both non-streaming generate() overloads.
 * @param result Completed result (mutated throughout).
 * @param model Backend that ran the decode.
 * @param resolved_params Params as the backend saw them.
 * @param selected Tier that ran.
 * @param routing_ms Router classification time.
 * @param swap_ms Model-swap time.
 * @param t_start Start of the whole orchestration.
 * @req REQ-INFER-008
 * @version 2.13.0
 */
void ModelOrchestrator::finish_generation(
    GenerationResult& result,
    InferenceBackend* model,
    const GenerationParams& resolved_params,
    const std::string& selected,
    double routing_ms,
    double swap_ms,
    std::chrono::steady_clock::time_point t_start)
{
    apply_adapter_parse(model, get_adapter(selected), result);
    // gh#134 (v2.10.4): name a budget-starved mandatory-tool turn.
    warn_turn_diagnostics(result, selected, config_.models.tiers);

    result.routing_ms = routing_ms;
    result.swap_ms = swap_ms;
    result.total_ms = elapsed_ms(t_start, now());
    // gh#154: one record per generation, at the tail every orchestrated
    // path already shares — so a new decode path cannot ship unrecorded.
    record_generation(result, resolved_params, model);
    log_orchestration(result, selected, last_routing_result_.adapter_name,
                      resolved_params, routing_ms, swap_ms);
}

/**
 * @brief Attach grammar provenance and append a metric record (gh#154).
 * @param result Completed result (mutated: `grammar` populated).
 * @param resolved_params Params as the backend saw them.
 * @param model Backend that ran the decode (may be null).
 * @req REQ-INFER-008
 * @version 2.13.0
 */
void ModelOrchestrator::record_generation(
    GenerationResult& result,
    const GenerationParams& resolved_params,
    const InferenceBackend* model)
{
    const std::string tool_grammar =
        model != nullptr ? model->active_tool_grammar() : std::string{};
    result.grammar = describe_grammar(resolved_params, tool_grammar);

    std::lock_guard<std::mutex> lock(records_mutex_);
    if (generation_records_.size() >= kMaxGenerationRecords) {
        generation_records_.erase(generation_records_.begin());
    }
    generation_records_.push_back(make_generation_record(result));
}

/**
 * @brief Per-generation metric records, oldest first (gh#154).
 * @return A copy of the ring, so the caller never holds the lock.
 * @req REQ-INFER-008
 * @version 2.13.0
 */
std::vector<GenerationRecord> ModelOrchestrator::generation_records() const
{
    std::lock_guard<std::mutex> lock(records_mutex_);
    return generation_records_;
}

/**
 * @brief Normalize a frontmatter grammar value to a registry key.
 *
 * Strips .gbnf extension if present: "compactor.gbnf" → "compactor".
 * Values without extension are used as-is.
 *
 * @param grammar_value Raw frontmatter value.
 * @return Normalized registry key.
 * @utility
 * @version 1.9.3
 */
static std::string normalize_grammar_key(const std::string& grammar_value) {
    std::filesystem::path p(grammar_value);
    if (p.extension() == ".gbnf") {
        return p.stem().string();
    }
    return grammar_value;
}

/**
 * @brief Resolve grammar_key to grammar content string in params.
 *
 * Resolution order:
 * 1. params.grammar (raw string) — highest priority, skip registry
 * 2. params.grammar_key — lookup in GrammarRegistry
 * 3. Identity frontmatter grammar: field — normalize and lookup
 * 4. None — unconstrained generation
 *
 * An unresolvable RUNTIME key (`params.grammar_key`) logs a warning and
 * leaves the decode unconstrained rather than failing the turn — it may
 * name a grammar registered after configure. A TIER's `grammar:` stem is
 * different: the ENGINE selects the tier, so a caller cannot see the miss,
 * and the resulting unconstrained decode looks exactly like a constrained
 * one. `refuse_unresolved_tier_grammar` — which calls this and then reads
 * what it recorded — turns that case into
 * `ENTROPIC_ERROR_GRAMMAR_NOT_FOUND` before anything decodes. Configure
 * only WARNS about it, because `entropic_grammar_register*` requires an
 * orchestrator and therefore cannot run until configure has returned.
 *
 * gh#154: the key and its origin are recorded on `params` whether or not
 * the lookup succeeds, so `GenerationResult::grammar` can report a named
 * key that constrained nothing.
 *
 * @param params Generation parameters (mutated: grammar + provenance).
 * @param tier_name Active tier for frontmatter grammar resolution.
 * @req REQ-INFER-007
 * @version 2.13.0
 */
void ModelOrchestrator::resolve_grammar_key(
    GenerationParams& params, const std::string& tier_name)
{
    if (!params.grammar.empty()) {
        return;
    }

    // Try explicit grammar_key
    std::string key = params.grammar_key;

    // Fall back to tier config grammar field (frontmatter)
    if (key.empty()) {
        auto it = config_.models.tiers.find(tier_name);
        if (it != config_.models.tiers.end() && it->second.grammar) {
            key = normalize_grammar_key(it->second.grammar->string());
            // gh#154: remember WHERE the key came from. All three
            // request-side sources arrive as params.grammar, so without
            // this the result cannot say whether the tier asked or the
            // caller did.
            params.grammar_from_tier = true;
        }
    }

    if (key.empty()) {
        return;
    }

    // Recorded BEFORE the lookup, so a key that resolves to nothing is
    // still named on the result. A missing grammar leaves the decode
    // unconstrained (REQ-INFER-007) and the absence of a log line was,
    // until gh#154, the only signal that it had happened.
    params.resolved_grammar_key = key;

    std::string content = grammar_registry_.get(key);
    if (content.empty()) {
        logger->warn("Grammar key '{}' not found in registry — this decode "
                     "is UNCONSTRAINED; read generations[].grammar.resolved "
                     "from entropic_metrics_json to detect it", key);
        return;
    }

    logger->info("Grammar resolved: key='{}', {} bytes",
                 key, content.size());
    params.grammar = std::move(content);
}

/**
 * @brief Refuse a dispatch whose TIER grammar is not registered (gh#154).
 *
 * See the header for why the refusal lives here rather than at configure.
 * The short version: `entropic_grammar_register*` needs an orchestrator,
 * which only exists after `entropic_configure*`, so refusing at configure
 * made "configure, then register this tier's grammar" — a documented
 * sequence, and the one `tests/model/test_gh95_identity_grammar.cpp`
 * exercises — impossible to perform.
 *
 * Loud, not silent, and not fatal to the process: the caller gets a typed
 * error naming the tier, the stem and the call that fixes it, and nothing
 * decodes. The alternative this replaces is the decode running
 * UNCONSTRAINED, which produces output of the same shape as a constrained
 * run whenever the prompt also describes the shape.
 *
 * @param params Dispatch params (mutated: grammar resolved).
 * @param tier_name Selected tier.
 * @return Refusal result, or std::nullopt when the dispatch may proceed.
 * @req REQ-INFER-007
 * @version 2.13.0
 */
std::optional<GenerationResult>
ModelOrchestrator::refuse_unresolved_tier_grammar(
    GenerationParams& params, const std::string& tier_name)
{
    resolve_grammar_key(params, tier_name);          // v1.9.3
    if (!tier_grammar_unresolved(params)) {
        return std::nullopt;
    }

    GenerationResult err;
    err.finish_reason = "error";
    err.error_code = ENTROPIC_ERROR_GRAMMAR_NOT_FOUND;
    err.error_message =
        "Tier '" + tier_name + "' declares grammar '"
        + params.resolved_grammar_key + "' and no grammar is registered "
        "under that key. Register it with entropic_grammar_register_file() "
        "or entropic_grammar_register() before running this tier, or place "
        + params.resolved_grammar_key + ".gbnf in a grammar search path. "
        "Refused rather than decoded unconstrained (gh#154): an "
        "unconstrained decode is indistinguishable from a constrained one "
        "in the output.";
    logger->error("{}", err.error_message);
    return err;
}

/**
 * @brief Resolve every batch arm's grammar, refusing the batch on a miss.
 *
 * A batch is one decode over a shared prefill, so it cannot run
 * half-constrained — one unresolved arm refuses all of them.
 *
 * @param params_list Per-request base params.
 * @param tiers Per-request tier names ("" = `lead`).
 * @param lead Lead tier name.
 * @param[out] out Grammar-resolved params, one per request.
 * @return Refusal result, or std::nullopt when every arm resolves.
 * @req REQ-INFER-007
 * @version 2.13.0
 */
std::optional<GenerationResult>
ModelOrchestrator::refuse_unresolved_batch_grammars(
    const std::vector<GenerationParams>& params_list,
    const std::vector<std::string>& tiers,
    const std::string& lead,
    std::vector<GenerationParams>& out)
{
    out.clear();
    out.reserve(params_list.size());
    for (std::size_t i = 0; i < params_list.size(); ++i) {
        GenerationParams resolved = params_list[i];
        auto refusal = refuse_unresolved_tier_grammar(
            resolved, batch_arm_tier(tiers, i, lead));
        if (refusal.has_value()) {
            return refusal;
        }
        out.push_back(std::move(resolved));
    }
    return std::nullopt;
}

namespace {
/**
 * @brief Apply a tier override iff set AND the field is still at its
 *        GenerationParams default (preserving a per-call override).
 * @utility
 * @version 2.5.3
 */
template <typename T>
inline void apply_if_default(T& field, const std::optional<T>& ov, T dflt) {
    if (ov.has_value() && field == dflt) { field = *ov; }
}
}  // namespace

/**
 * @brief Pure precedence helper — see header. (gh#82/gh#85)
 *
 * Each field takes the tier's value only when the tier set one AND the
 * caller left that field at its GenerationParams default — so a per-call
 * knob is never overwritten by tier config.
 *
 * @param params Generation parameters (mutated in place).
 * @param ov The tier's optional sampler overrides.
 * @req REQ-INFER-021
 * @version 2.8.2
 */
void apply_tier_sampler_overrides(
    GenerationParams& params, const TierSamplerOverrides& ov)
{
    // GenerationParams struct defaults (see types/config.h).
    apply_if_default(params.temperature,       ov.temperature,       0.7f);
    apply_if_default(params.max_tokens,        ov.max_output_tokens, 4096);
    apply_if_default(params.top_p,             ov.top_p,             0.9f);
    apply_if_default(params.top_k,             ov.top_k,             40);
    apply_if_default(params.min_p,             ov.min_p,             0.0f);
    apply_if_default(params.presence_penalty,  ov.presence_penalty,  0.0f);
    apply_if_default(params.frequency_penalty, ov.frequency_penalty, 0.0f);
    apply_if_default(params.repeat_penalty,    ov.repeat_penalty,    1.1f);  // gh#86
    apply_if_default(params.enable_thinking,   ov.enable_thinking,   true);  // gh#86
    apply_if_default(params.tool_call_mode,    ov.tool_call_mode, std::string{});  // gh#103
}

/**
 * @brief Apply per-tier sampler config to params. (gh#82, v2.4.4)
 *
 * Member wrapper: looks the tier up in config and delegates the
 * precedence decision to the free `apply_tier_sampler_overrides`, which
 * applies a tier value only where the caller left the struct default —
 * an explicit caller knob always wins.
 *
 * @param params Generation parameters (mutated in place).
 * @param tier_name Tier whose overrides to consult; an unknown tier is a
 *        no-op.
 * @req REQ-INFER-021
 * @version 2.8.2
 */
void ModelOrchestrator::apply_tier_sampler_defaults(
    GenerationParams& params, const std::string& tier_name)
{
    auto it = config_.models.tiers.find(tier_name);
    if (it == config_.models.tiers.end()) { return; }
    const auto& tier = it->second;
    TierSamplerOverrides ov;
    ov.temperature       = tier.temperature;
    ov.max_output_tokens = tier.max_output_tokens;
    ov.top_p             = tier.top_p;              // gh#85
    ov.top_k             = tier.top_k;              // gh#85
    ov.min_p             = tier.min_p;              // gh#85
    ov.presence_penalty  = tier.presence_penalty;   // gh#85
    ov.frequency_penalty = tier.frequency_penalty;  // gh#85
    ov.repeat_penalty    = tier.repeat_penalty;     // gh#86
    ov.enable_thinking   = tier.enable_thinking;    // gh#86
    ov.tool_call_mode    = tier.tool_call_mode;     // gh#103
    float before_temp = params.temperature;
    int before_max = params.max_tokens;
    apply_tier_sampler_overrides(params, ov);
    if (params.temperature != before_temp) {
        logger->info("Tier '{}' temperature applied: {}",
                     tier_name, params.temperature);
    }
    if (params.max_tokens != before_max) {
        logger->info("Tier '{}' max_output_tokens applied: {}",
                     tier_name, params.max_tokens);
    }
}

// ── VRAM-aware tier residency (v2.2.4, gh#57) ──────────────

/**
 * @brief Resolve the VRAM budget: env override, else the device.
 *
 * SET wins, always — including empty or unparseable, which resolve to 0 and
 * DISABLE the gate. That is the operator's escape hatch back to pre-gh#142
 * behaviour, and it is the v2.3.10 contract; the device fallback must never
 * override an explicit setting. Only an ABSENT variable reaches the device,
 * which itself returns 0 (gate disabled) when there is no GPU.
 *
 * @return Budget in bytes, or 0 meaning "unknown, do not enforce".
 * @dg_internal
 * @req REQ-INFER-019
 * @version 2.11.1
 */
size_t ModelOrchestrator::resolve_vram_budget_bytes() {
    const char* env = std::getenv("ENTROPIC_VRAM_BUDGET_BYTES");
    if (env == nullptr) {
        // Not set at all: fall through to the device.
        return static_cast<size_t>(query_device_free_vram_bytes());
    }
    // SET, so it takes control — including when it is empty or unparseable,
    // which resolve to 0 and therefore DISABLE the gate. That is the escape
    // hatch for an operator who wants the pre-gh#142 behaviour back, and it is
    // what the v2.3.10 contract already specified; the device fallback must not
    // quietly override an explicit setting.
    size_t budget = 0;
    if (*env != '\0') {
        try {
            long long v = std::stoll(env);
            budget = (v < 0) ? 0 : static_cast<size_t>(v);
        } catch (...) {
            budget = 0;
        }
    }
    return budget;
}


/**
 * @brief Gather a tier's footprint inputs for the pure estimator.
 *
 * Resolves the vision projector's size when the tier declares one — it is the
 * allocation that actually aborted the process in gh#142 and the v2.2.4
 * estimate ignored it entirely.
 *
 * @param tier_cfg The tier's configuration.
 * @param weights_bytes Size of the tier's GGUF on disk.
 * @return Inputs for estimate_vram_footprint.
 * @dg_internal
 * @req REQ-INFER-019
 * @version 2.12.0
 */
static FootprintInputs footprint_inputs_for(
    const TierConfig& tier_cfg, uint64_t weights_bytes, int vram_reserve_mb) {
    FootprintInputs in;
    in.weights_bytes = weights_bytes;
    in.gpu_layers = tier_cfg.gpu_layers;
    in.context_length = tier_cfg.context_length;
    in.cache_type_k = tier_cfg.cache_type_k;
    in.cache_type_v = tier_cfg.cache_type_v;
    in.vram_reserve_mb = vram_reserve_mb;
    // gh#144 (v2.12.0): context_length is per session, so the pool's KV term
    // multiplies. Omitting this under-counts by exactly N and can admit a
    // configuration that aborts the host — what this gate exists to prevent.
    in.max_sessions = tier_cfg.max_sessions;
    if (!tier_cfg.mmproj_path.empty()) {
        std::error_code proj_ec;
        auto proj = std::filesystem::file_size(tier_cfg.mmproj_path, proj_ec);
        if (!proj_ec) { in.mmproj_bytes = proj; }
    }
    return in;
}

/**
 * @brief Estimate per-tier VRAM footprint.
 *
 * Delegates the arithmetic to the pure, CPU-unit-tested estimator in
 * vram_footprint.h, which prices weights by their offload placement, KV by its
 * cache type, and counts the vision projector. Returns 0 when the tier or its
 * GGUF is not resolvable, AND when the placement cannot be priced at all —
 * both mean "unknown" to the gate, which then does not enforce.
 *
 * @dg_internal
 * @req REQ-INFER-019
 * @version 2.11.0
 */
size_t ModelOrchestrator::estimate_footprint_bytes(
    const std::string& tier_name) const {
    auto tier_it = config_.models.tiers.find(tier_name);
    if (tier_it == config_.models.tiers.end()) { return 0; }
    const auto& tier_cfg = tier_it->second;
    std::error_code ec;
    auto weights = std::filesystem::file_size(tier_cfg.path, ec);
    if (ec) { return 0; }
    FootprintInputs in = footprint_inputs_for(
        tier_cfg, weights, config_.vram_reserve_mb);
    // gh#142: an unpriceable placement returns 0 = "unknown", which leaves the
    // gate open. Guessing here would refuse working configurations — a 13 GB
    // model at gpu_layers=15 runs fine on an 11 GB card.
    return static_cast<size_t>(estimate_vram_footprint(in).bytes);
}

/**
 * @brief Public footprint accessor — memoizes via tier_footprint_bytes_.
 * @dg_internal
 * @version 2.2.4
 */
size_t ModelOrchestrator::tier_footprint_bytes(
    const std::string& tier_name) const {
    std::lock_guard<std::mutex> lock(swap_mutex_);
    auto it = tier_footprint_bytes_.find(tier_name);
    if (it != tier_footprint_bytes_.end()) { return it->second; }
    size_t v = estimate_footprint_bytes(tier_name);
    if (v > 0) {
        tier_footprint_bytes_[tier_name] = v;
    }
    return v;
}

/**
 * @brief Register / replace / clear the residency observer.
 * @dg_internal
 * @version 2.2.4
 */
void ModelOrchestrator::set_residency_observer(ResidencyObserverFn cb) {
    std::lock_guard<std::mutex> lock(swap_mutex_);
    residency_observer_ = std::move(cb);
}

/**
 * @brief Fire residency observer + INFO-log the event.
 * @dg_internal
 * @version 2.2.4
 */
void ModelOrchestrator::fire_residency_observer(
    ResidencyEvent event,
    const std::string& tier_name,
    const std::string& model_path,
    size_t footprint) {
    const char* event_name = "unknown";
    switch (event) {
    case ResidencyEvent::Loaded:         event_name = "loaded"; break;
    case ResidencyEvent::Evicted:        event_name = "evicted"; break;
    case ResidencyEvent::ActivationSwap: event_name = "activation_swap"; break;
    }
    logger->info("[residency] {} tier='{}' path='{}' footprint={} bytes",
                 event_name, tier_name, model_path, footprint);
    if (residency_observer_) {
        residency_observer_(event, tier_name, model_path, footprint);
    }
}

/**
 * @brief JSON serialization of the current residency set.
 * @dg_internal
 * @version 2.2.4
 */
/**
 * @brief Build one residency-snapshot JSON entry.
 * @param name Tier name.
 * @param path Model file path.
 * @param context_length Tier context length (for KV estimate).
 * @param footprint Resolved footprint bytes.
 * @param vram_reserve_mb Configured headroom reserve.
 * @param last_ms Last activation time (ms).
 * @return JSON entry object.
 * @utility
 * @version 2.3.7
 */
static nlohmann::json make_residency_entry(
    const std::string& name, const std::filesystem::path& path,
    int context_length, size_t footprint, int vram_reserve_mb,
    long long last_ms) {
    std::error_code ec;
    auto weights = std::filesystem::file_size(path, ec);
    size_t weights_b = ec ? 0u : static_cast<size_t>(weights);
    size_t kv = static_cast<size_t>(context_length) * 16ull * 1024ull;
    size_t headroom = static_cast<size_t>(vram_reserve_mb)
        * 1024ull * 1024ull;
    return {
        {"tier",               name},
        {"model_path",         path.string()},
        {"footprint_bytes",    footprint},
        {"weights_bytes",      weights_b},
        {"kv_cache_bytes",     kv},
        {"headroom_bytes",     headroom},
        {"last_activation_ms", last_ms}
    };
}

/**
 * @brief Serialize the current VRAM residency snapshot to JSON.
 * @dg_internal
 * @version 2.3.7
 */
std::string ModelOrchestrator::residency_snapshot_json() const {
    std::lock_guard<std::mutex> lock(swap_mutex_);
    nlohmann::json j;
    j["vram_total_bytes"]     = vram_budget_bytes_;
    j["vram_budget_bytes"]    = vram_budget_bytes_;
    size_t in_use = 0;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [name, backend] : tiers_) {
        if (!backend || !backend->is_loaded()) { continue; }
        auto tier_it = config_.models.tiers.find(name);
        if (tier_it == config_.models.tiers.end()) { continue; }
        auto fp_it = tier_footprint_bytes_.find(name);
        size_t footprint = (fp_it != tier_footprint_bytes_.end())
            ? fp_it->second : estimate_footprint_bytes(name);
        in_use += footprint;
        auto la = tier_last_activation_ms_.find(name);
        long long last_ms = (la != tier_last_activation_ms_.end())
            ? la->second : 0;
        arr.push_back(make_residency_entry(
            name, tier_it->second.path, tier_it->second.context_length,
            footprint, config_.vram_reserve_mb, last_ms));
    }
    j["residency"] = std::move(arr);
    j["vram_headroom_bytes"] = vram_budget_bytes_ > in_use
        ? vram_budget_bytes_ - in_use
        : 0u;
    j["backend"] = vram_budget_bytes_ > 0 ? "configured" : "unknown";
    return j.dump();
}

} // namespace entropic
