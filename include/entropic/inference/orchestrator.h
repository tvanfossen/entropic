/**
 * @file orchestrator.h
 * @brief ModelOrchestrator — multi-model lifecycle and routing.
 *
 * @par Responsibilities
 * - Model pool deduplication (one backend per unique .gguf path)
 * - Per-tier adapters (identity-specific, independent of shared backend)
 * - VRAM lifecycle: one ACTIVE main tier, router always ACTIVE
 * - Tier routing via router model (raw completion, digit classification)
 * - Handoff rule enforcement
 * - Swap logic: keep_warm → WARM, otherwise → COLD
 *
 * @par Thread safety
 * Uses std::mutex for tier swap operations. State queries on individual
 * backends are lock-free (atomic). Generation calls are not serialized.
 *
 * Internal to inference .so.
 *
 * @version 1.8.2
 */

#pragma once

#include <entropic/inference/backend.h>
#include <entropic/inference/adapters/adapter_base.h>
#include <entropic/types/config.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace entropic {

/**
 * @brief Result metadata from a routing decision.
 * @version 1.8.2
 */
struct RoutingResult {
    std::string tier_name;                ///< Selected tier
    std::string previous_tier;            ///< Previous tier (empty if first)
    std::string model_raw;                ///< Raw model output (e.g. "2")
    std::string swap_action = "none";     ///< "none", "reused", "loaded"
    double routing_ms = 0.0;              ///< Total routing time
};

/**
 * @brief Multi-model lifecycle and routing orchestrator.
 *
 * Manages model pool deduplication, per-tier adapters, VRAM lifecycle,
 * and digit-based tier routing via a router model.
 *
 * @version 1.8.2
 */
class ModelOrchestrator {
public:
    /**
     * @brief Initialize from parsed config.
     * @param config Full engine config.
     * @return true on success.
     * @version 1.8.2
     */
    bool initialize(const ParsedConfig& config);

    /**
     * @brief Shutdown — unload all models.
     * @version 1.8.2
     */
    void shutdown();

    /* ── Generation ──────────────────────────────────────── */

    /**
     * @brief Generate using routed or explicit tier.
     * @param messages Conversation history.
     * @param params Generation parameters.
     * @param tier_name Explicit tier, or empty for routing.
     * @return GenerationResult with routing/swap timing.
     * @version 1.8.2
     */
    GenerationResult generate(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        const std::string& tier_name = "");

    /**
     * @brief Streaming generation.
     * @version 1.8.2
     */
    GenerationResult generate_streaming(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view)> on_token,
        std::atomic<bool>& cancel,
        const std::string& tier_name = "");

    /* ── Routing ─────────────────────────────────────────── */

    /**
     * @brief Route to tier using router model.
     * @param messages Current conversation.
     * @return Selected tier name.
     * @version 1.8.2
     */
    std::string route(const std::vector<Message>& messages);

    /* ── Queries ─────────────────────────────────────────── */

    /**
     * @brief Last routing result.
     * @version 1.8.2
     */
    RoutingResult last_routing_result() const;

    /**
     * @brief Last used tier name.
     * @version 1.8.2
     */
    std::string last_used_tier() const;

    /**
     * @brief Currently loaded model tier names.
     * @version 1.8.2
     */
    std::vector<std::string> loaded_models() const;

    /**
     * @brief All configured tier names.
     * @version 1.8.2
     */
    std::vector<std::string> available_models() const;

    /**
     * @brief Check if handoff is permitted.
     * @version 1.8.2
     */
    bool can_handoff(const std::string& from, const std::string& to) const;

    /**
     * @brief Get adapter for a tier.
     * @version 1.8.2
     */
    ChatAdapter* get_adapter(const std::string& tier_name) const;

private:
    /* ── Model pool (one backend per unique path) ────────── */
    std::unordered_map<std::string, std::shared_ptr<InferenceBackend>> model_pool_;

    /* ── Tier → backend mapping (many-to-one) ────────────── */
    std::unordered_map<std::string, std::shared_ptr<InferenceBackend>> tiers_;

    /* ── Per-tier adapters (one-to-one, identity-specific) ── */
    std::unordered_map<std::string, std::unique_ptr<ChatAdapter>> adapters_;

    /* ── Router (separate small model, always ACTIVE) ────── */
    std::shared_ptr<InferenceBackend> router_;

    /* ── Routing state ───────────────────────────────────── */
    std::unordered_map<std::string, std::string> tier_map_;  ///< digit → tier name
    std::unordered_map<std::string, std::unordered_set<std::string>> handoff_rules_;
    std::string default_tier_;
    std::string loaded_main_tier_;
    RoutingResult last_routing_result_;
    std::vector<std::string> tier_history_;  ///< Recent tiers, max 5

    std::mutex swap_mutex_;  ///< Guards tier swap operations

    ParsedConfig config_;

    /* ── Internal ────────────────────────────────────────── */

    /**
     * @brief Get model for tier, loading/swapping as needed.
     * @version 1.8.2
     */
    InferenceBackend* get_model(const std::string& tier_name);

    /**
     * @brief Deactivate current main tier for swap.
     * @version 1.8.2
     */
    void deactivate_current_if_needed(InferenceBackend* incoming);

    /**
     * @brief Classify task using router model.
     * @version 1.8.2
     */
    std::pair<std::string, std::string> classify_task(
        const std::vector<Message>& messages);
};

} // namespace entropic
