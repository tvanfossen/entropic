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
 * enabled` is true and a `draft_model` is configured.
 *
 * @param config Parsed engine config.
 * @return true on success.
 * @utility
 * @version 2.1.11
 */
bool ModelOrchestrator::initialize(const ParsedConfig& config) {
    config_ = config;
    default_tier_ = config.models.default_tier;

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
 * @brief Generate response using routed or explicit tier.
 *
 * Speculative routing added in v2.1.11 (gh#36): when the kernel is
 * configured and the target/draft pair is compatible, dispatches
 * through `LlamaCppBackend::generate_speculative_with_draft`; falls
 * back to plain decode otherwise. The dispatch decision is delegated
 * to `run_generate_dispatch` to keep this method under the SLOC gate.
 *
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param tier_name Explicit tier or empty for routing.
 * @return GenerationResult.
 * @internal
 * @version 2.1.11
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

    if (!model) {
        GenerationResult err;
        err.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        err.error_message = "No model available for tier: " + selected;
        err.finish_reason = "error";
        return err;
    }

    // Resolve grammar_key → grammar content (v1.9.3)
    GenerationParams resolved_params = params;
    resolve_grammar_key(resolved_params, selected);

    // Generate — speculative routing applies here too (v2.1.11, gh#36)
    GenerationResult result = run_generate_dispatch(
        model, messages, resolved_params);

    auto* adapter = get_adapter(selected);
    if (adapter && !result.content.empty()) {
        result.raw_content = result.content;
        auto parsed = adapter->parse_tool_calls(result.content);
        result.content = parsed.cleaned_content;
        result.tool_calls = std::move(parsed.tool_calls);
    }

    result.routing_ms = routing_ms;
    result.swap_ms = swap_ms;
    result.total_ms = elapsed_ms(t_start, now());
    logger->info("Orchestration: tier={}, adapter={}, grammar={}",
                 selected, last_routing_result_.adapter_name,
                 resolved_params.grammar.empty()
                     ? "unconstrained" : resolved_params.grammar_key);
    logger->info("Total: {:.0f}ms (route={:.0f}ms, swap={:.0f}ms, "
                 "gen={:.0f}ms)",
                 result.total_ms, routing_ms, swap_ms,
                 result.generation_time_ms);
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
InferenceBackend* ModelOrchestrator::get_model(const std::string& tier_name) {
    std::lock_guard<std::mutex> lock(swap_mutex_);

    auto it = tiers_.find(tier_name);
    if (it == tiers_.end()) {
        it = tiers_.find(config_.routing.fallback_tier);
    }

    InferenceBackend* result = nullptr;

    if (it == tiers_.end()) {
        /* no tier found — result stays nullptr */
    } else if (it->second->is_active()) {
        last_routing_result_.swap_action = "reused";
        result = it->second.get();
    } else {
        auto& backend = it->second;
        deactivate_current_if_needed(backend.get());

        auto tier_it = config_.models.tiers.find(tier_name);
        bool activated = tier_it != config_.models.tiers.end()
            && backend->load_and_activate(tier_it->second);

        if (activated) {
            loaded_main_tier_ = tier_name;
            last_routing_result_.swap_action = "loaded";
            result = backend.get();
        } else {
            logger->error("Failed to activate tier: {}", tier_name);
        }
    }

    // Ensure correct LoRA adapter for this tier (v1.9.2)
    if (result) {
        auto* llama_backend = dynamic_cast<LlamaCppBackend*>(result);
        llama_context* ctx = llama_backend
            ? llama_backend->llama_context_ptr() : nullptr;
        double adapter_ms = ensure_adapter_for_tier(tier_name, ctx);
        last_routing_result_.adapter_swap_ms = adapter_ms;
        last_routing_result_.adapter_name = lora_manager_.active_adapter();
    }

    return result;
}

/**
 * @brief Deactivate current main tier for swap.
 *
 * keep_warm=true → WARM. keep_warm=false → COLD.
 *
 * @param incoming The backend about to be activated.
 * @internal
 * @version 1.9.2
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

    auto& current = it->second;
    auto cfg_it = config_.models.tiers.find(loaded_main_tier_);
    bool keep_warm = cfg_it != config_.models.tiers.end() && cfg_it->second.keep_warm;

    // Cascade: unload adapters for this base model (v1.9.2)
    auto* llama_backend = dynamic_cast<LlamaCppBackend*>(current.get());
    if (llama_backend) {
        lora_manager_.unload_all_for_model(
            llama_backend->llama_model_ptr(),
            llama_backend->llama_context_ptr());
    }

    if (keep_warm) {
        logger->info("Deactivating {} (keep_warm=true)", loaded_main_tier_);
        current->deactivate();
    } else {
        logger->info("Unloading {} (keep_warm=false)", loaded_main_tier_);
        current->unload();
    }
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

} // namespace entropic
