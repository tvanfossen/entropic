// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file secondary_model_loader.cpp
 * @brief SecondaryModelLoader implementation.
 *
 * Role-keyed lifecycle for non-primary inference backends. Absorbs the
 * lifecycle previously hosted directly on ModelOrchestrator::router_;
 * extends to draft (v2.1.11) and future thinking-model (gh#25) slots.
 *
 * @version 2.1.11
 */

#include <entropic/inference/secondary_model_loader.h>
#include <entropic/types/logging.h>

#include "llama_cpp_backend.h"

#include <algorithm>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.secondary_loader");
} // anonymous namespace

/**
 * @brief Lazily load and activate a model for a role.
 * @param role Role name (e.g. `"router"`, `"draft"`).
 * @param config ModelConfig for the secondary model.
 * @return true on activation success.
 * @internal
 * @version 2.1.11
 */
bool SecondaryModelLoader::ensure_loaded(
    const std::string& role, const ModelConfig& config) {
    std::lock_guard<std::mutex> lock(slots_mutex_);

    const std::string new_path = config.path.string();
    auto path_it = slot_paths_.find(role);
    if (path_it != slot_paths_.end() && path_it->second == new_path) {
        auto it = slots_.find(role);
        if (it != slots_.end() && it->second->is_loaded()) {
            return true;
        }
    }

    auto backend = std::make_shared<LlamaCppBackend>();
    if (!backend->load_and_activate(config)) {
        logger->error("Failed to activate role '{}' from path: {}",
                      role, new_path);
        return false;
    }

    slots_[role] = backend;
    slot_paths_[role] = new_path;
    logger->info("Activated secondary role '{}' from {}", role, new_path);
    return true;
}

/**
 * @brief Get the backend for a role.
 * @param role Role name.
 * @return Backend pointer, nullptr if role is unknown.
 * @utility
 * @version 2.1.11
 */
InferenceBackend* SecondaryModelLoader::get(const std::string& role) const {
    std::lock_guard<std::mutex> lock(slots_mutex_);
    auto it = slots_.find(role);
    return (it == slots_.end()) ? nullptr : it->second.get();
}

/**
 * @brief Get the backend for a role as a shared_ptr.
 * @param role Role name.
 * @return Backend shared_ptr, empty if role is unknown.
 * @utility
 * @version 2.1.11
 */
std::shared_ptr<InferenceBackend> SecondaryModelLoader::get_shared(
    const std::string& role) const {
    std::lock_guard<std::mutex> lock(slots_mutex_);
    auto it = slots_.find(role);
    return (it == slots_.end()) ? std::shared_ptr<InferenceBackend>{}
                                : it->second;
}

/**
 * @brief Unload and drop a role.
 * @param role Role name.
 * @return true if a role was unloaded, false if none was loaded.
 * @internal
 * @version 2.1.11
 */
bool SecondaryModelLoader::release_role(const std::string& role) {
    std::lock_guard<std::mutex> lock(slots_mutex_);
    auto it = slots_.find(role);
    if (it == slots_.end()) {
        return false;
    }
    if (it->second->is_loaded()) {
        it->second->unload();
    }
    slots_.erase(it);
    slot_paths_.erase(role);
    logger->info("Released secondary role '{}'", role);
    return true;
}

/**
 * @brief Check whether a role is currently loaded and non-COLD.
 * @param role Role name.
 * @return true if backend is present and is_loaded().
 * @utility
 * @version 2.1.11
 */
bool SecondaryModelLoader::is_loaded(const std::string& role) const {
    std::lock_guard<std::mutex> lock(slots_mutex_);
    auto it = slots_.find(role);
    return it != slots_.end() && it->second->is_loaded();
}

/**
 * @brief Names of all loaded roles (sorted for deterministic output).
 * @return Sorted role names whose backend reports is_loaded().
 * @utility
 * @version 2.1.11
 */
std::vector<std::string> SecondaryModelLoader::loaded_roles() const {
    std::lock_guard<std::mutex> lock(slots_mutex_);
    std::vector<std::string> out;
    out.reserve(slots_.size());
    for (const auto& [role, backend] : slots_) {
        if (backend->is_loaded()) {
            out.push_back(role);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/**
 * @brief Fanout: clear prompt/KV cache on every loaded backend.
 * @utility
 * @version 2.1.11
 */
void SecondaryModelLoader::clear_all_prompt_caches() {
    std::lock_guard<std::mutex> lock(slots_mutex_);
    for (auto& [role, backend] : slots_) {
        backend->clear_prompt_cache();
    }
}

/**
 * @brief Unload every role. Safe to call repeatedly.
 * @internal
 * @version 2.1.11
 */
void SecondaryModelLoader::shutdown() {
    std::lock_guard<std::mutex> lock(slots_mutex_);
    for (auto& [role, backend] : slots_) {
        if (backend->is_loaded()) {
            backend->unload();
        }
    }
    slots_.clear();
    slot_paths_.clear();
}

} // namespace entropic
