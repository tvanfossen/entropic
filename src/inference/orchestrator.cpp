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
#include "adapters/adapter_registry.h"

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
 * @internal
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
 * @internal
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
 * @param config Parsed engine config.
 * @utility
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
 * @param config Parsed engine config.
 * @return true on success, false on activation failure.
 * @utility
 * @version 2.0.2
 */
bool ModelOrchestrator::activate_default_tier(const ParsedConfig& config) {
    if (tiers_.find(default_tier_) == tiers_.end()) { return true; }
    auto& backend = tiers_[default_tier_];
    auto& tier_cfg = config.models.tiers.at(default_tier_);
    if (!backend->load_and_activate(tier_cfg)) {
        logger->error("Failed to activate default tier: {}", default_tier_);
        return false;
    }
    loaded_main_tier_ = default_tier_;
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
 * @utility
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
 * @internal
 * @version 2.1.11 [reviewed]
 */
void ModelOrchestrator::activate_draft(const ParsedConfig& config) {
    const auto& spec = config.inference.speculative;
    if (!spec.enabled || spec.draft.path.empty()) { return; }
    // Full ModelConfig comes from the YAML's
    // `inference.speculative.draft:` block — every llama.cpp knob is
    // consumer-tunable. Defaults come from
    // `make_default_draft_model_config()` (gpu_layers=0,
    // flash_attn=false, context_length=8192, n_threads=4).
    secondary_loader_.ensure_loaded("draft", spec.draft);
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
 * @version 2.2.4
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

    // Route ggml/llama logs before any model loading
    if (config.ggml_logging && !config.log_dir.empty()) {
        auto path = (config.log_dir / "llama_ggml.log").string();
        entropic_inference_log_to_file(path.c_str());
        logger->info("ggml logging: {}", path);
    }

    logger->info("Initializing model orchestrator");

    if (!create_tier_backends(config)) { return false; }
    build_routing_tables(config);
    if (!activate_default_tier(config)) { return false; }
    activate_router(config);
    activate_draft(config);      // Speculative draft slot (v2.1.11)

    preload_adapters();          // LoRA adapters → WARM (v1.9.2)
    load_bundled_grammars();     // Bundled grammars (v1.9.3)
    return true;
}

/**
 * @brief Shutdown — unload all models.
 *
 * Main-tier pool is unloaded directly; secondary roles (router, draft,
 * etc.) are released through `secondary_loader_.shutdown()` (v2.1.11).
 *
 * @internal
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
 * @utility
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
 * @brief Run a generate call through speculative (if enabled+pair
 *        compatible) or fall back to plain decode.
 * @internal
 * @version 2.1.11
 */
GenerationResult ModelOrchestrator::run_generate_dispatch(
    InferenceBackend* model,
    const std::vector<Message>& messages,
    const GenerationParams& params) {
    GenerationResult result;
    bool kernel_ran = config_.inference.speculative.enabled
        && try_speculative_route(model, messages, params, result);
    if (!kernel_ran) {
        result = model->generate(messages, params);
    }
    return result;
}

/**
 * @brief Common implementation: returns true if the speculative
 *        kernel ran (result populated), false to fall back to plain.
 *
 * Centralises the dynamic_cast + compat check shared by both
 * generate and generate_streaming (v2.1.11, gh#36).
 *
 * @internal
 * @version 2.1.11
 */
bool ModelOrchestrator::try_speculative_route_streaming(
    InferenceBackend* model,
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view)> on_token,
    std::atomic<bool>& cancel,
    GenerationResult& result)
{
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
 * @internal
 * @version 2.1.11
 */
bool ModelOrchestrator::try_speculative_route(
    InferenceBackend* model,
    const std::vector<Message>& messages,
    const GenerationParams& params,
    GenerationResult& result)
{
    std::atomic<bool> local_cancel{false};
    return try_speculative_route_streaming(
        model, messages, params,
        [](std::string_view){}, local_cancel, result);
}

// ── Generation ─────────────────────────────────────────────

/**
 * @brief Run the tier's adapter over a result to split tool calls.
 * @param adapter Adapter for the selected tier (may be null).
 * @param[in,out] result Generation result (content split, tools set).
 * @utility
 * @version 2.3.7
 */
static void apply_adapter_parse(ChatAdapter* adapter,
                                GenerationResult& result) {
    if (!adapter || result.content.empty()) { return; }
    result.raw_content = result.content;
    auto parsed = adapter->parse_tool_calls(result.content);
    result.content = parsed.cleaned_content;
    result.tool_calls = std::move(parsed.tool_calls);
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
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param tier_name Explicit tier or empty for routing.
 * @return GenerationResult.
 * @internal
 * @version 2.3.7
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

    // Get model (may trigger swap)
    auto t_swap = now();
    InferenceBackend* model = get_model(selected);
    double swap_ms = elapsed_ms(t_swap, now());

    if (!model) { return build_no_model_error(selected); }

    // Resolve grammar_key → grammar content (v1.9.3)
    GenerationParams resolved_params = params;
    resolve_grammar_key(resolved_params, selected);

    // Generate — speculative routing applies here too (v2.1.11, gh#36)
    GenerationResult result = run_generate_dispatch(
        model, messages, resolved_params);

    apply_adapter_parse(get_adapter(selected), result);

    result.routing_ms = routing_ms;
    result.swap_ms = swap_ms;
    result.total_ms = elapsed_ms(t_start, now());
    log_orchestration(result, selected, last_routing_result_.adapter_name,
                      resolved_params, routing_ms, swap_ms);
    return result;
}

/**
 * @brief Streaming generation with speculative dispatch.
 *
 * Speculative routing added in v2.1.11 (gh#36): when the kernel is
 * configured and the target/draft pair is compatible, dispatches to
 * `LlamaCppBackend::generate_speculative_with_draft` via
 * `try_speculative_route_streaming` with the draft resolved from
 * `secondary_loader_.get("draft")`. Falls back to plain streaming on
 * NOT_SUPPORTED or compatibility failure, with a diagnostic logged.
 *
 * @internal
 * @version 2.1.11
 */
GenerationResult ModelOrchestrator::generate_streaming(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view)> on_token,
    std::atomic<bool>& cancel,
    const std::string& tier_name)
{
    std::string selected = tier_name.empty() ? route(messages) : tier_name;
    InferenceBackend* model = get_model(selected);

    if (!model) {
        GenerationResult err;
        err.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        err.error_message = "No model for tier: " + selected;
        err.finish_reason = "error";
        return err;
    }

    // Resolve grammar_key → grammar content (v1.9.3)
    GenerationParams resolved_params = params;
    resolve_grammar_key(resolved_params, selected);

    // Speculative routing (v2.1.11, gh#36): when speculative is
    // enabled in config AND target/draft pair is compatible, attempt
    // the speculative kernel. On NOT_SUPPORTED (kernel staged), fall
    // back to plain streaming. This keeps the v2.1.11 ship-without-
    // kernel state observable as "plain decode, speculative
    // requested but deferred."
    GenerationResult spec_streaming;
    if (config_.inference.speculative.enabled
        && try_speculative_route_streaming(
               model, messages, resolved_params, on_token, cancel,
               spec_streaming)) {
        return spec_streaming;
    }

    return model->generate_streaming(messages, resolved_params, on_token, cancel);
}

// ── Routing ────────────────────────────────────────────────

/**
 * @brief Route to appropriate tier using router model.
 *
 * Guard updated in v2.1.11: routing requires `models.router` to be
 * configured (was: `router_` non-null). The slot is owned by
 * `secondary_loader_` since gh#27.
 *
 * @param messages Current conversation.
 * @return Selected tier name.
 * @internal
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
 * @param messages Conversation history.
 * @return Pair of (tier_name, raw_digit), or ("","") on miss.
 * @internal
 * @version 2.1.11
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
    auto result = router_backend->complete(
        user_msg + " ->", router_params);
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
 * @internal
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
 * @internal
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

/**
 * @brief VRAM-budget admission test (gh#57).
 *
 * Estimates the tier's footprint, memoizes it, and rejects with
 * `last_residency_error_ = TIER_MODEL_TOO_LARGE` when the single-tier
 * estimate exceeds a known engine VRAM budget. Returns true to admit.
 *
 * @internal
 * @version 2.2.4
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
 * @internal
 * @version 2.2.4
 */
/**
 * @brief Build a typed GenerationResult for a get_model() failure.
 *
 * Threads the orchestrator's `last_residency_error_` stash into the
 * result so the facade surfaces `TIER_MODEL_TOO_LARGE` distinctly from
 * generic `GENERATE_FAILED`. Always clears the stash.
 *
 * @internal
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
 * @internal
 * @version 2.2.4
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
 * @internal
 * @version 2.3.7
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
    }

    return result;
}

/**
 * @brief Ensure the active tier's LoRA adapter is loaded.
 * @param tier_name Tier whose adapter to ensure.
 * @param result Active backend.
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
 * @version 1.8.2
 */
RoutingResult ModelOrchestrator::last_routing_result() const {
    return last_routing_result_;
}

/**
 * @brief Last used tier name.
 * @internal
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
 * @internal
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
 * @internal
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
 * @brief Check if handoff is permitted.
 * @internal
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
 * @internal
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
 * @internal
 * @version 1.9.2
 */
/**
 * @brief Deactivate any active LoRA adapter.
 * @param ctx llama_context to clear from.
 * @return true if an adapter was deactivated.
 * @internal
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
 * @internal
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
 * @brief Preload all tier-configured LoRA adapters to WARM.
 *
 * Scans tier configs for adapter_path. For each, loads the adapter
 * against its base model. Requires the base model to be at least WARM.
 *
 * @internal
 * @version 1.9.2
 */
void ModelOrchestrator::preload_adapters() {
    int loaded = 0;

    for (const auto& [name, tier_cfg] : config_.models.tiers) {
        if (!tier_cfg.adapter_path) {
            continue;
        }

        auto tier_it = tiers_.find(name);
        if (tier_it == tiers_.end()) {
            continue;
        }

        auto* llama_backend = dynamic_cast<LlamaCppBackend*>(
            tier_it->second.get());
        if (!llama_backend || !llama_backend->llama_model_ptr()) {
            logger->warn("Cannot preload adapter for '{}' — model not loaded",
                        name);
            continue;
        }

        bool ok = lora_manager_.load(
            name,
            *tier_cfg.adapter_path,
            llama_backend->llama_model_ptr(),
            tier_cfg.adapter_scale);

        if (ok) {
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
 * @internal
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
 * @internal
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
 * @utility
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
 * @return true if any configured tier declares "vision".
 * @internal
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
 * @return Tier name, or "" if none configured.
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @param params Generation parameters (mutated: grammar field may be set).
 * @param tier_name Active tier for frontmatter grammar resolution.
 * @internal
 * @version 2.0.0
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
        }
    }

    if (key.empty()) {
        return;
    }

    std::string content = grammar_registry_.get(key);
    if (content.empty()) {
        logger->warn("Grammar key '{}' not found in registry", key);
        return;
    }

    logger->info("Grammar resolved: key='{}', {} bytes",
                 key, content.size());
    params.grammar = std::move(content);
}

// ── VRAM-aware tier residency (v2.2.4, gh#57) ──────────────

/**
 * @brief Read ENTROPIC_VRAM_BUDGET_BYTES env override.
 *
 * Returns the parsed value (decimal bytes) on success, 0 when the
 * variable is unset, empty, or fails to parse. 0 means "budget
 * unknown, gate disabled" — see `get_model` for the gate semantics.
 *
 * @internal
 * @version 2.2.4
 */
size_t ModelOrchestrator::resolve_vram_budget_bytes() {
    const char* env = std::getenv("ENTROPIC_VRAM_BUDGET_BYTES");
    if (env == nullptr || *env == '\0') { return 0; }
    try {
        long long v = std::stoll(env);
        return (v < 0) ? 0 : static_cast<size_t>(v);
    } catch (...) {
        return 0;
    }
}

/**
 * @brief Estimate per-tier VRAM footprint.
 *
 * Weights file size + context_length × 16 KiB per-token KV estimate +
 * vram_reserve_mb × 1MiB headroom. Returns 0 when the tier or its
 * GGUF file is not resolvable. Pure metadata — no model load.
 *
 * @internal
 * @version 2.2.4
 */
size_t ModelOrchestrator::estimate_footprint_bytes(
    const std::string& tier_name) const {
    auto tier_it = config_.models.tiers.find(tier_name);
    if (tier_it == config_.models.tiers.end()) { return 0; }
    const auto& tier_cfg = tier_it->second;
    std::error_code ec;
    auto weights = std::filesystem::file_size(tier_cfg.path, ec);
    if (ec) { return 0; }
    const size_t kv_per_token = 16ull * 1024ull;
    size_t kv = static_cast<size_t>(tier_cfg.context_length) * kv_per_token;
    size_t headroom = static_cast<size_t>(config_.vram_reserve_mb)
        * 1024ull * 1024ull;
    return static_cast<size_t>(weights) + kv + headroom;
}

/**
 * @brief Public footprint accessor — memoizes via tier_footprint_bytes_.
 * @internal
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
 * @internal
 * @version 2.2.4
 */
void ModelOrchestrator::set_residency_observer(ResidencyObserverFn cb) {
    std::lock_guard<std::mutex> lock(swap_mutex_);
    residency_observer_ = std::move(cb);
}

/**
 * @brief Fire residency observer + INFO-log the event.
 * @internal
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
 * @internal
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
 * @internal
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
