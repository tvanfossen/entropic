/**
 * @file adapter_manager.cpp
 * @brief AdapterManager implementation — LoRA lifecycle and hot-swap.
 *
 * Integrates with llama.cpp LoRA C API (pinned b8420):
 * - llama_adapter_lora_init() — load adapter against model
 * - llama_set_adapters_lora() — set active adapters on context
 * - llama_adapter_lora_free() — release adapter
 * - llama_memory_clear() — clear KV cache after swap
 *
 * @version 1.9.2
 */

#include <entropic/inference/adapter_manager.h>
#include <entropic/types/logging.h>

#include <llama.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.adapter_manager");

/**
 * @brief Get steady clock time point.
 * @utility
 * @version 1.9.2
 */
auto now() { return std::chrono::steady_clock::now(); }

/**
 * @brief Compute elapsed milliseconds between two time points.
 * @param start Start time.
 * @param end End time.
 * @return Elapsed milliseconds.
 * @utility
 * @version 1.9.2
 */
double elapsed_ms(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end)
{
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    return static_cast<double>(us.count()) / 1000.0;
}

/**
 * @brief Apply a single adapter to a context via llama_set_adapters_lora.
 * @param ctx llama_context.
 * @param adapter llama_adapter_lora handle.
 * @param scale LoRA scaling factor.
 * @utility
 * @version 1.9.2
 */
void apply_adapter(llama_context* ctx,
                   llama_adapter_lora* adapter, float scale)
{
    llama_set_adapters_lora(ctx, &adapter, 1, &scale);
}

/**
 * @brief Clear all adapters from a context.
 * @param ctx llama_context.
 * @utility
 * @version 1.9.2
 */
void clear_adapters(llama_context* ctx) {
    llama_set_adapters_lora(ctx, nullptr, 0, nullptr);
}

} // anonymous namespace

// ── Load ────────────────────────────────────────────────────

/**
 * @brief Load a LoRA adapter into RAM (COLD -> WARM).
 *
 * Calls llama_adapter_lora_init() to load the adapter against
 * the base model. The adapter stays in RAM until explicitly
 * unloaded or the base model is unloaded.
 *
 * @param name Unique identifier.
 * @param adapter_path Path to .gguf adapter file.
 * @param model Base llama_model pointer.
 * @param scale LoRA scaling factor.
 * @return true on success, false on duplicate name or load failure.
 * @internal
 * @version 1.9.2
 */
bool AdapterManager::load(
    const std::string& name,
    const std::filesystem::path& adapter_path,
    llama_model* model,
    float scale)
{
    std::lock_guard<std::mutex> lock(adapter_mutex_);

    bool precondition_failed =
        adapters_.find(name) != adapters_.end() || !model;
    if (precondition_failed) {
        logger->error("Cannot load adapter '{}': {}",
                     name, !model ? "base model is null" : "duplicate name");
        return false;
    }

    auto t_start = now();
    auto* lora = llama_adapter_lora_init(model, adapter_path.c_str());
    if (!lora) {
        logger->error("llama_adapter_lora_init failed for '{}' at {}",
                     name, adapter_path.string());
        return false;
    }

    AdapterEntry entry;
    entry.name = name;
    entry.path = adapter_path;
    entry.handle = lora;
    entry.model = model;
    entry.scale = scale;
    entry.state = AdapterState::WARM;
    adapters_.emplace(name, std::move(entry));

    logger->info("Loaded adapter '{}' from {} in {:.1f}ms (scale={:.2f})",
                name, adapter_path.string(),
                elapsed_ms(t_start, now()), scale);
    return true;
}

// ── Unload ──────────────────────────────────────────────────

/**
 * @brief Unload adapter (any state -> COLD).
 *
 * If HOT, clears from context first. Frees via llama_adapter_lora_free().
 * No-op if not found.
 *
 * @param name Adapter identifier.
 * @param ctx Context to clear from (if HOT). May be nullptr.
 * @internal
 * @version 1.9.2
 */
void AdapterManager::unload(const std::string& name, llama_context* ctx) {
    std::lock_guard<std::mutex> lock(adapter_mutex_);

    auto it = adapters_.find(name);
    if (it == adapters_.end()) {
        return;
    }

    auto& entry = it->second;
    if (entry.state == AdapterState::HOT && ctx) {
        clear_adapters(ctx);
        active_name_.clear();
    }

    if (entry.handle) {
        llama_adapter_lora_free(entry.handle);
        entry.handle = nullptr;
    }

    entry.state = AdapterState::COLD;
    logger->info("Unloaded adapter '{}'", name);

    adapters_.erase(it);
}

// ── Activate ────────────────────────────────────────────────

/**
 * @brief Activate adapter on context (WARM -> HOT).
 *
 * If another adapter is HOT, deactivates it first. Uses
 * llama_set_adapters_lora() to apply the single adapter.
 *
 * @param name Adapter identifier.
 * @param ctx llama_context to activate on.
 * @return true on success.
 * @internal
 * @version 1.9.2
 */
bool AdapterManager::activate(const std::string& name, llama_context* ctx) {
    std::lock_guard<std::mutex> lock(adapter_mutex_);

    auto it = adapters_.find(name);
    bool cannot_activate =
        it == adapters_.end() || it->second.state == AdapterState::COLD;
    if (it != adapters_.end() && it->second.state == AdapterState::HOT) {
        return true;  // Already active — no-op
    }
    if (cannot_activate) {
        logger->error("Cannot activate adapter '{}': {}",
                     name, it == adapters_.end() ? "not found" : "state is COLD");
        return false;
    }

    auto& entry = it->second;

    // Mark previous HOT adapter as WARM
    if (!active_name_.empty() && active_name_ != name) {
        auto active_it = adapters_.find(active_name_);
        if (active_it != adapters_.end()) {
            active_it->second.state = AdapterState::WARM;
        }
    }

    apply_adapter(ctx, entry.handle, entry.scale);
    entry.state = AdapterState::HOT;
    active_name_ = name;

    logger->info("Activated adapter '{}' (scale={:.2f})", name, entry.scale);
    return true;
}

// ── Deactivate ──────────────────────────────────────────────

/**
 * @brief Deactivate current HOT adapter (HOT -> WARM).
 *
 * Clears all adapters from the context. No-op if none active.
 *
 * @param ctx llama_context to clear from.
 * @internal
 * @version 1.9.2
 */
void AdapterManager::deactivate(llama_context* ctx) {
    std::lock_guard<std::mutex> lock(adapter_mutex_);

    if (active_name_.empty()) {
        return;
    }

    auto it = adapters_.find(active_name_);
    if (it != adapters_.end()) {
        if (ctx) {
            clear_adapters(ctx);
        }
        it->second.state = AdapterState::WARM;
    }

    logger->info("Deactivated adapter '{}'", active_name_);
    active_name_.clear();
}

// ── Swap ────────────────────────────────────────────────────

/**
 * @brief Swap to a different adapter atomically.
 *
 * Deactivates current HOT adapter and activates the target.
 * Fires ON_ADAPTER_SWAP hook which can cancel the operation.
 *
 * @param name Target adapter (must be WARM).
 * @param ctx llama_context to swap on.
 * @return true on success.
 * @internal
 * @version 1.9.2
 */
bool AdapterManager::swap(const std::string& name, llama_context* ctx) {
    std::lock_guard<std::mutex> lock(adapter_mutex_);

    auto it = adapters_.find(name);
    bool cannot_swap = it == adapters_.end()
                    || it->second.state == AdapterState::COLD;
    if (active_name_ == name && !cannot_swap) {
        return true;  // Already active — no-op
    }
    if (cannot_swap || !fire_swap_hook(active_name_, name, it->second.path)) {
        logger->error("Cannot swap to adapter '{}': {}",
                     name, cannot_swap ? "not found or COLD" : "cancelled by hook");
        return false;
    }

    auto t_start = now();

    // Mark current HOT as WARM
    if (!active_name_.empty()) {
        auto active_it = adapters_.find(active_name_);
        if (active_it != adapters_.end()) {
            active_it->second.state = AdapterState::WARM;
        }
    }

    // Apply target
    auto& target = it->second;
    apply_adapter(ctx, target.handle, target.scale);
    target.state = AdapterState::HOT;

    std::string previous = active_name_;
    active_name_ = name;

    logger->info("Swapped adapter '{}' -> '{}' in {:.1f}ms",
                previous, name, elapsed_ms(t_start, now()));
    return true;
}

// ── Unload All ──────────────────────────────────────────────

/**
 * @brief Unload all adapters targeting a specific base model.
 *
 * Called when base model transitions out of ACTIVE/WARM.
 * Prevents dangling adapter handles.
 *
 * @param model The base model being unloaded.
 * @param ctx Context to clear from. May be nullptr.
 * @internal
 * @version 1.9.2
 */
void AdapterManager::unload_all_for_model(
    llama_model* model, llama_context* ctx)
{
    std::lock_guard<std::mutex> lock(adapter_mutex_);

    bool cleared_context = false;
    std::vector<std::string> to_remove;

    for (auto& [name, entry] : adapters_) {
        if (entry.model != model) {
            continue;
        }

        if (entry.state == AdapterState::HOT
            && ctx && !cleared_context)
        {
            clear_adapters(ctx);
            cleared_context = true;
        }

        if (entry.handle) {
            llama_adapter_lora_free(entry.handle);
            entry.handle = nullptr;
        }

        entry.state = AdapterState::COLD;
        to_remove.push_back(name);
    }

    for (const auto& name : to_remove) {
        if (name == active_name_) {
            active_name_.clear();
        }
        adapters_.erase(name);
    }

    logger->info("Unloaded {} adapter(s) for model", to_remove.size());
}

// ── Queries ─────────────────────────────────────────────────

/**
 * @brief Get adapter state.
 * @param name Adapter identifier.
 * @return AdapterState. COLD if not found.
 * @internal
 * @version 1.9.2
 */
AdapterState AdapterManager::state(const std::string& name) const {
    std::lock_guard<std::mutex> lock(adapter_mutex_);
    auto it = adapters_.find(name);
    if (it == adapters_.end()) {
        return AdapterState::COLD;
    }
    return it->second.state;
}

/**
 * @brief Get metadata for an adapter.
 * @param name Adapter identifier.
 * @return AdapterInfo. COLD with empty name if not found.
 * @internal
 * @version 1.9.2
 */
AdapterInfo AdapterManager::info(const std::string& name) const {
    std::lock_guard<std::mutex> lock(adapter_mutex_);
    auto it = adapters_.find(name);
    if (it == adapters_.end()) {
        return {};
    }
    return make_info(it->second);
}

/**
 * @brief List all known adapters.
 * @return Vector of AdapterInfo snapshots.
 * @internal
 * @version 1.9.2
 */
std::vector<AdapterInfo> AdapterManager::list_adapters() const {
    std::lock_guard<std::mutex> lock(adapter_mutex_);
    std::vector<AdapterInfo> result;
    result.reserve(adapters_.size());
    for (const auto& [name, entry] : adapters_) {
        result.push_back(make_info(entry));
    }
    return result;
}

/**
 * @brief Get currently HOT adapter name.
 * @return Adapter name, empty if none.
 * @internal
 * @version 1.9.2
 */
std::string AdapterManager::active_adapter() const {
    std::lock_guard<std::mutex> lock(adapter_mutex_);
    return active_name_;
}

/**
 * @brief Set hook dispatch interface.
 * @param hooks Hook interface from facade.
 * @internal
 * @version 1.9.2
 */
void AdapterManager::set_hook_interface(const HookInterface& hooks) {
    hooks_ = hooks;
}

// ── Private ─────────────────────────────────────────────────

/**
 * @brief Build AdapterInfo from internal entry.
 * @param entry Internal adapter entry.
 * @return AdapterInfo snapshot.
 * @internal
 * @version 1.9.2
 */
AdapterInfo AdapterManager::make_info(const AdapterEntry& entry) {
    AdapterInfo info;
    info.name = entry.name;
    info.path = entry.path;
    info.state = entry.state;
    info.scale = entry.scale;
    info.tier_name = entry.tier_name;
    info.ram_bytes = entry.ram_bytes;
    info.metadata = entry.metadata;
    return info;
}

/**
 * @brief Fire ON_ADAPTER_SWAP pre-hook.
 *
 * Constructs JSON context and fires through the hook interface.
 * Returns false if the hook cancelled the swap.
 *
 * @param current Current HOT adapter name.
 * @param target Target adapter name.
 * @param target_path Target .gguf path.
 * @return true if swap should proceed.
 * @internal
 * @version 1.9.2
 */
bool AdapterManager::fire_swap_hook(
    const std::string& current,
    const std::string& target,
    const std::filesystem::path& target_path)
{
    if (!hooks_.fire_pre || !hooks_.registry) {
        return true;  // No hook registered — proceed
    }

    nlohmann::json ctx;
    ctx["current_adapter"] = current;
    ctx["target_adapter"] = target;
    ctx["adapter_path"] = target_path.string();
    std::string ctx_str = ctx.dump();

    char* modified = nullptr;
    int rc = hooks_.fire_pre(
        hooks_.registry,
        ENTROPIC_HOOK_ON_ADAPTER_SWAP,
        ctx_str.c_str(),
        &modified);

    if (modified) {
        free(modified);
    }

    return rc == 0;
}

} // namespace entropic
