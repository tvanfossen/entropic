/**
 * @file adapter_manager.h
 * @brief AdapterManager — LoRA adapter lifecycle and hot-swap.
 *
 * @par Responsibilities
 * - Load/unload LoRA adapters against a base model
 * - Activate/deactivate adapters on an inference context
 * - Track adapter states (COLD/WARM/HOT)
 * - Enforce single-HOT constraint per context
 * - Provide adapter metadata for routing
 *
 * @par Thread safety
 * - State queries require the class mutex (AdapterEntry::state)
 * - load/unload/swap acquire adapter_mutex_
 * - Swap operations coordinate with InferenceBackend transition_mutex_
 *   via the orchestrator (AdapterManager does NOT lock the backend)
 *
 * @par Ownership
 * Owned by ModelOrchestrator. One AdapterManager per engine instance.
 *
 * @version 1.9.2
 */

#pragma once

#include <entropic/types/config.h>
#include <entropic/interfaces/i_hook_handler.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations (llama.cpp types — not in public header)
struct llama_model;
struct llama_context;
struct llama_adapter_lora;

namespace entropic {

/**
 * @brief LoRA adapter lifecycle manager.
 *
 * Manages adapter COLD/WARM/HOT states. One active (HOT) adapter per
 * context at a time. Multiple adapters can be WARM simultaneously.
 *
 * @par Lifecycle
 * @code
 *   COLD ──load()──> WARM ──activate()──> HOT
 *     ^                ^                    |
 *     └──unload()──────┘<──deactivate()─────┘
 * @endcode
 *
 * @version 1.9.2
 */
class AdapterManager {
public:
    /**
     * @brief Load a LoRA adapter into RAM (COLD -> WARM).
     * @param name Unique identifier for this adapter.
     * @param adapter_path Path to the LoRA .gguf file.
     * @param model llama_model the adapter targets.
     * @param scale LoRA scaling factor (default 1.0).
     * @return true on success.
     * @version 1.9.2
     */
    bool load(const std::string& name,
              const std::filesystem::path& adapter_path,
              llama_model* model,
              float scale = 1.0f);

    /**
     * @brief Unload adapter (any state -> COLD).
     * @param name Adapter identifier.
     * @param ctx Context to deactivate from (if HOT). May be nullptr.
     * @version 1.9.2
     */
    void unload(const std::string& name, llama_context* ctx);

    /**
     * @brief Activate adapter on context (WARM -> HOT).
     * @param name Adapter identifier.
     * @param ctx llama_context to activate on.
     * @return true on success.
     * @version 1.9.2
     */
    bool activate(const std::string& name, llama_context* ctx);

    /**
     * @brief Deactivate current HOT adapter (HOT -> WARM).
     * @param ctx llama_context to clear adapter from.
     * @version 1.9.2
     */
    void deactivate(llama_context* ctx);

    /**
     * @brief Swap to a different adapter atomically.
     * @param name Target adapter (must be WARM).
     * @param ctx llama_context to swap on.
     * @return true on success.
     * @version 1.9.2
     */
    bool swap(const std::string& name, llama_context* ctx);

    /**
     * @brief Unload all adapters for a given base model.
     * @param model The base model being unloaded.
     * @param ctx Context to deactivate from. May be nullptr.
     * @version 1.9.2
     */
    void unload_all_for_model(llama_model* model, llama_context* ctx);

    /* ── Queries (lock-free where possible) ───────────── */

    /**
     * @brief Get adapter state.
     * @param name Adapter identifier.
     * @return AdapterState. COLD if not found.
     * @version 1.9.2
     */
    AdapterState state(const std::string& name) const;

    /**
     * @brief Get metadata for an adapter.
     * @param name Adapter identifier.
     * @return AdapterInfo. COLD state if not found.
     * @version 1.9.2
     */
    AdapterInfo info(const std::string& name) const;

    /**
     * @brief List all known adapters.
     * @return Vector of AdapterInfo.
     * @version 1.9.2
     */
    std::vector<AdapterInfo> list_adapters() const;

    /**
     * @brief Get the currently HOT adapter name.
     * @return Adapter name, empty if none active.
     * @version 1.9.2
     */
    std::string active_adapter() const;

    /**
     * @brief Set hook interface for ON_ADAPTER_SWAP dispatch.
     * @param hooks Hook dispatch interface.
     * @version 1.9.2
     */
    void set_hook_interface(const HookInterface& hooks);

private:
    /**
     * @brief Internal entry for a managed adapter.
     * @version 1.9.2
     */
    struct AdapterEntry {
        std::string name;                          ///< Adapter identifier
        std::filesystem::path path;                ///< .gguf file path
        llama_adapter_lora* handle = nullptr;      ///< llama.cpp adapter handle
        llama_model* model = nullptr;              ///< Base model this targets
        float scale = 1.0f;                        ///< LoRA scale factor
        AdapterState state = AdapterState::COLD; ///< Lifecycle state (mutex-protected)
        size_t ram_bytes = 0;                      ///< RAM consumed
        std::string tier_name;                     ///< Assigned tier
        std::unordered_map<std::string, std::string> metadata; ///< Routing metadata
    };

    /**
     * @brief Build AdapterInfo from an entry.
     * @param entry Internal adapter entry.
     * @return AdapterInfo snapshot.
     * @version 1.9.2
     */
    static AdapterInfo make_info(const AdapterEntry& entry);

    /**
     * @brief Fire ON_ADAPTER_SWAP pre-hook.
     * @param current Current HOT adapter name.
     * @param target Target adapter name.
     * @param target_path Target adapter file path.
     * @return true if hook allows the swap (not cancelled).
     * @version 1.9.2
     */
    bool fire_swap_hook(const std::string& current,
                        const std::string& target,
                        const std::filesystem::path& target_path);

    std::unordered_map<std::string, AdapterEntry> adapters_; ///< All managed adapters
    std::string active_name_;           ///< Currently HOT adapter (empty if none)
    mutable std::mutex adapter_mutex_;  ///< Guards transitions and map
    HookInterface hooks_;               ///< Hook dispatch (optional)
};

} // namespace entropic
