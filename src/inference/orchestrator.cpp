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
 * the same .gguf share a single backend instance.
 *
 * @param config Full engine config.
 * @return true on success.
 * @internal
 * @version 1.9.3
 */
bool ModelOrchestrator::initialize(const ParsedConfig& config) {
    config_ = config;
    default_tier_ = config.models.default_tier;

    logger->info("Initializing model orchestrator");

    // Create deduplicated backends per unique model path
    for (const auto& [name, tier_config] : config.models.tiers) {
        std::string path_key = tier_config.path.string();

        if (model_pool_.find(path_key) == model_pool_.end()) {
            auto backend = std::make_shared<LlamaCppBackend>();
            model_pool_[path_key] = backend;
        }
        tiers_[name] = model_pool_[path_key];

        // Per-tier adapter (identity-specific)
        adapters_[name] = create_adapter(
            tier_config.adapter, name, "" /* prompt resolved later */);
    }

    logger->info("Created {} unique backend(s) for {} tier(s)",
              model_pool_.size(), tiers_.size());

    // Router backend (separate small model)
    if (config.models.router) {
        router_ = std::make_shared<LlamaCppBackend>();
    }

    // Build routing data
    for (const auto& [digit, tier_name] : config.routing.tier_map) {
        tier_map_[digit] = tier_name;
    }
    for (const auto& [src, targets] : config.routing.handoff_rules) {
        handoff_rules_[src] = std::unordered_set<std::string>(
            targets.begin(), targets.end());
    }

    // Load default tier
    if (tiers_.find(default_tier_) != tiers_.end()) {
        auto& backend = tiers_[default_tier_];
        auto& tier_cfg = config.models.tiers.at(default_tier_);
        if (backend->load_and_activate(tier_cfg)) {
            loaded_main_tier_ = default_tier_;
            logger->info("Activated default tier: {}", default_tier_);
        } else {
            logger->error("Failed to activate default tier: {}", default_tier_);
            return false;
        }
    }

    // Load router
    if (router_ && config.models.router) {
        if (!router_->load_and_activate(*config.models.router)) {
            logger->error("Failed to activate router model");
        } else {
            logger->info("Activated router model");
        }
    }

    // Preload LoRA adapters to WARM (v1.9.2)
    preload_adapters();

    // Load bundled grammars (v1.9.3)
    load_bundled_grammars();

    return true;
}

/**
 * @brief Shutdown — unload all models.
 * @internal
 * @version 1.8.2
 */
void ModelOrchestrator::shutdown() {
    logger->info("Shutting down model orchestrator");

    for (auto& [path, backend] : model_pool_) {
        if (backend->is_loaded()) {
            backend->unload();
        }
    }

    if (router_ && router_->is_loaded()) {
        router_->unload();
    }
}

// ── Generation ─────────────────────────────────────────────

/**
 * @brief Generate response using routed or explicit tier.
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param tier_name Explicit tier or empty for routing.
 * @return GenerationResult.
 * @internal
 * @version 2.0.0
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

    // Generate
    auto result = model->generate(messages, resolved_params);

    // Parse tool calls via adapter
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
 * @brief Streaming generation.
 * @internal
 * @version 1.9.3
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

    return model->generate_streaming(messages, resolved_params, on_token, cancel);
}

// ── Routing ────────────────────────────────────────────────

/**
 * @brief Route to appropriate tier using router model.
 * @param messages Current conversation.
 * @return Selected tier name.
 * @internal
 * @version 2.0.0
 */
std::string ModelOrchestrator::route(const std::vector<Message>& messages) {
    if (!config_.routing.enabled || !router_) {
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
 * @param messages Conversation history.
 * @return Pair of (tier_name, raw_digit).
 * @internal
 * @version 2.0.0
 */
std::pair<std::string, std::string> ModelOrchestrator::classify_task(
    const std::vector<Message>& messages)
{
    std::string user_msg = extract_latest_user_message(messages);

    GenerationParams router_params;
    router_params.max_tokens = 1;
    router_params.temperature = 0.0f;

    auto result = router_->complete(user_msg + " ->", router_params);
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
 * @internal
 * @version 1.8.2
 */
std::vector<std::string> ModelOrchestrator::loaded_models() const {
    std::vector<std::string> result;
    for (const auto& [name, backend] : tiers_) {
        if (backend->is_loaded()) {
            result.push_back(name);
        }
    }
    if (router_ && router_->is_loaded()) {
        result.push_back("router");
    }
    return result;
}

/**
 * @brief All configured tier names.
 * @internal
 * @version 1.8.2
 */
std::vector<std::string> ModelOrchestrator::available_models() const {
    std::vector<std::string> result;
    for (const auto& [name, _] : tiers_) {
        result.push_back(name);
    }
    if (router_) {
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
 * @version 1.9.3
 */
void ModelOrchestrator::load_bundled_grammars() {
    std::filesystem::path grammar_dir;
    if (!config_.config_dir.empty()) {
        grammar_dir = config_.config_dir / "grammars";
    }

    if (grammar_dir.empty() || !std::filesystem::is_directory(grammar_dir)) {
        logger->info("No bundled grammar directory found, skipping");
        return;
    }

    size_t count = grammar_registry_.load_bundled(grammar_dir);
    logger->info("Grammar registry: {} grammar(s) loaded", count);
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
