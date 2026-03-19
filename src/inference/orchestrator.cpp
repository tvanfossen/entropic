/**
 * @file orchestrator.cpp
 * @brief ModelOrchestrator implementation.
 *
 * Model pool deduplication, per-tier adapters, VRAM lifecycle,
 * tier routing via router complete(), and swap logic.
 *
 * @version 1.8.2
 */

#include <entropic/inference/orchestrator.h>
#include <entropic/types/logging.h>

#include "llama_cpp_backend.h"
#include "adapters/adapter_registry.h"

#include <chrono>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.orchestrator");

/**
 * @brief Get current time for timing measurements.
 * @utility
 * @version 1.8.2
 */
auto now() { return std::chrono::steady_clock::now(); }

/**
 * @brief Compute elapsed milliseconds.
 * @utility
 * @version 1.8.2
 */
double elapsed_ms(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end)
{
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    return static_cast<double>(us.count()) / 1000.0;
}

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
 * @version 1.8.2
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
 * @version 1.8.2
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

    // Generate
    auto result = model->generate(messages, params);

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
    return result;
}

/**
 * @brief Streaming generation.
 * @internal
 * @version 1.8.2
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

    return model->generate_streaming(messages, params, on_token, cancel);
}

// ── Routing ────────────────────────────────────────────────

/**
 * @brief Route to appropriate tier using router model.
 * @param messages Current conversation.
 * @return Selected tier name.
 * @internal
 * @version 1.8.2
 */
std::string ModelOrchestrator::route(const std::vector<Message>& messages) {
    if (!config_.routing.enabled || !router_) {
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
 * @version 1.8.2
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
            return {it->second, digit};
        }
    }

    logger->warn("[ROUTER] No valid digit in '{}', defaulting to {}", raw, default_tier_);
    return {default_tier_, ""};
}

// ── Model access ───────────────────────────────────────────

/**
 * @brief Get model for tier, loading/swapping as needed.
 * @param tier_name Tier name.
 * @return Backend pointer, or nullptr if unavailable.
 * @internal
 * @version 1.8.2
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

    return result;
}

/**
 * @brief Deactivate current main tier for swap.
 *
 * keep_warm=true → WARM. keep_warm=false → COLD.
 *
 * @param incoming The backend about to be activated.
 * @internal
 * @version 1.8.2
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

} // namespace entropic
