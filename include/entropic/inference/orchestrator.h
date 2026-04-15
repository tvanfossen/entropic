// SPDX-License-Identifier: LGPL-3.0-or-later
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
#include <entropic/inference/adapter_manager.h>
#include <entropic/inference/grammar_registry.h>
#include <entropic/inference/profile_registry.h>
#include <entropic/inference/throughput_tracker.h>
#include <entropic/inference/adapters/adapter_base.h>
#include <entropic/types/config.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct llama_context;  // Forward declaration for adapter management

namespace entropic {

/**
 * @brief Result metadata from a routing decision.
 * @version 1.9.2 — added adapter_name, adapter_swap_ms
 */
struct RoutingResult {
    std::string tier_name;                ///< Selected tier
    std::string previous_tier;            ///< Previous tier (empty if first)
    std::string model_raw;                ///< Raw model output (e.g. "2")
    std::string swap_action = "none";     ///< "none", "reused", "loaded"
    double routing_ms = 0.0;              ///< Total routing time
    std::string adapter_name;             ///< Active adapter (empty = base model) (v1.9.2)
    double adapter_swap_ms = 0.0;         ///< Adapter swap latency (v1.9.2)
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

    /**
     * @brief Get the inference backend for a tier (for evaluation APIs).
     * @param tier_name Tier name (e.g. "lead", "eng").
     * @return Backend pointer, or nullptr if tier not found.
     * @version 1.10.2
     */
    InferenceBackend* get_backend(const std::string& tier_name) const;

    /**
     * @brief Access the LoRA adapter manager.
     * @return Reference to AdapterManager.
     * @utility
     * @version 1.9.2
     */
    AdapterManager& adapter_manager() { return lora_manager_; }

    /**
     * @brief Access the grammar registry.
     * @return Reference to GrammarRegistry.
     * @utility
     * @version 1.9.3
     */
    GrammarRegistry& grammar_registry() { return grammar_registry_; }

    /**
     * @brief Access the GPU resource profile registry.
     * @return Reference to ProfileRegistry.
     * @utility
     * @version 2.0.0
     */
    ProfileRegistry& profile_registry() { return profile_registry_; }

    /**
     * @brief Access the throughput tracker.
     * @return Reference to ThroughputTracker.
     * @utility
     * @version 2.0.0
     */
    ThroughputTracker& throughput_tracker() { return throughput_tracker_; }

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

    /* ── LoRA adapter management (v1.9.2) ────────────────── */
    AdapterManager lora_manager_;  ///< LoRA adapter lifecycle

    /* ── Grammar registry (v1.9.3) ────────────────────────── */
    GrammarRegistry grammar_registry_;  ///< Named grammar storage and resolution

    /* ── Profile registry (v2.0.0) ───────────────────────── */
    ProfileRegistry profile_registry_;  ///< Named GPU resource profiles

    /* ── Throughput tracker (v2.0.0) ─────────────────────── */
    ThroughputTracker throughput_tracker_;  ///< EWMA throughput measurement

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

    /**
     * @brief Deactivate any active LoRA adapter.
     * @param ctx llama_context to clear from.
     * @return true if an adapter was deactivated.
     * @version 1.9.2
     */
    bool deactivate_if_active(llama_context* ctx);

    /**
     * @brief Ensure correct LoRA adapter is active for a tier.
     *
     * If tier has adapter_path, activates it. If tier has no
     * adapter_path, deactivates any active adapter.
     *
     * @param tier_name Target tier.
     * @param ctx llama_context for activation.
     * @return Adapter swap time in ms (0 if no swap needed).
     * @version 1.9.2
     */
    double ensure_adapter_for_tier(
        const std::string& tier_name, llama_context* ctx);

    /**
     * @brief Preload all configured tier adapters to WARM.
     * @version 1.9.2
     */
    void preload_adapters();

    /**
     * @brief Build per-tier backends and adapters from config.
     * @param config Full engine config.
     * @return true on success, false if any tier model file is missing.
     * @internal
     * @version 2.0.2
     */
    bool create_tier_backends(const ParsedConfig& config);

    /**
     * @brief Build routing maps (tier_map_, handoff_rules_) from config.
     * @param config Full engine config.
     * @internal
     * @version 2.0.2
     */
    void build_routing_tables(const ParsedConfig& config);

    /**
     * @brief Activate the default tier (and load if not yet loaded).
     * @param config Full engine config.
     * @return true on success.
     * @internal
     * @version 2.0.2
     */
    bool activate_default_tier(const ParsedConfig& config);

    /**
     * @brief Activate the router model if configured.
     * @param config Full engine config.
     * @internal
     * @version 2.0.2
     */
    void activate_router(const ParsedConfig& config);

    /**
     * @brief Load bundled grammars from data directory at startup.
     * @version 1.9.3
     */
    void load_bundled_grammars();

    /**
     * @brief Resolve grammar_key to grammar content string.
     * @param params Generation params (mutated).
     * @param tier_name Active tier for frontmatter fallback.
     * @version 1.9.3
     */
    void resolve_grammar_key(GenerationParams& params,
                             const std::string& tier_name);
};

} // namespace entropic
