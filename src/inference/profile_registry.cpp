/**
 * @file profile_registry.cpp
 * @brief ProfileRegistry implementation -- named GPU resource profiles.
 *
 * @version 1.9.7
 */

#include <entropic/inference/profile_registry.h>
#include <entropic/types/logging.h>

#include <algorithm>
#include <thread>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.profile_registry");
} // anonymous namespace

/**
 * @brief Initialize with bundled profiles.
 *
 * Computes hardware-dependent thread counts for the background profile
 * at call time. If hardware_concurrency() returns 0 (unknown platform),
 * falls back to 2 threads.
 *
 * @internal
 * @version 1.9.7
 */
void ProfileRegistry::load_bundled() {
    std::lock_guard<std::mutex> lock(mutex_);
    profiles_.clear();

    unsigned int hw_threads = std::thread::hardware_concurrency();
    int bg_threads = (hw_threads > 0)
        ? std::max(1u, hw_threads / 2)
        : 2;

    profiles_["maximum"] = GPUResourceProfile{
        "maximum", 2048, 0, 0,
        "Foreground generation, latency-sensitive"};

    profiles_["balanced"] = GPUResourceProfile{
        "balanced", 512, 0, 0,
        "Default. Good throughput without monopolizing"};

    profiles_["background"] = GPUResourceProfile{
        "background", 256, bg_threads, bg_threads,
        "Background tasks (compaction, scribe)"};

    profiles_["minimal"] = GPUResourceProfile{
        "minimal", 64, 2, 2,
        "Lowest resource usage (background NPCs, idle)"};

    logger->info("Loaded 4 bundled GPU resource profiles "
                 "(background n_threads={})", bg_threads);
}

/**
 * @brief Register a custom profile.
 * @param profile Profile to register. profile.name is the key.
 * @return true on success. false if name already exists.
 * @internal
 * @version 1.9.7
 */
bool ProfileRegistry::register_profile(const GPUResourceProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (profiles_.count(profile.name) > 0) {
        logger->warn("Profile '{}' already registered", profile.name);
        return false;
    }

    profiles_[profile.name] = profile;
    logger->info("Registered profile '{}' (n_batch={}, n_threads={})",
                 profile.name, profile.n_batch, profile.n_threads);
    return true;
}

/**
 * @brief Remove a profile by name.
 * @param name Profile name.
 * @return true if removed. false if not found.
 * @internal
 * @version 1.9.7
 */
bool ProfileRegistry::deregister(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = profiles_.find(name);
    if (it == profiles_.end()) {
        return false;
    }
    profiles_.erase(it);
    logger->info("Deregistered profile '{}'", name);
    return true;
}

/**
 * @brief Get a profile by name with fallback.
 * @param name Profile name.
 * @return Profile struct. Falls back to "balanced" with WARNING on miss.
 * @internal
 * @version 1.9.7
 */
GPUResourceProfile ProfileRegistry::get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = profiles_.find(name);
    if (it != profiles_.end()) {
        return it->second;
    }

    logger->warn("Profile '{}' not found, falling back to 'balanced'", name);
    auto fallback = profiles_.find("balanced");
    if (fallback != profiles_.end()) {
        return fallback->second;
    }

    logger->error("Fallback profile 'balanced' also missing");
    return GPUResourceProfile{};
}

/**
 * @brief Check if a profile name exists.
 * @param name Profile name.
 * @return true if registered.
 * @internal
 * @version 1.9.7
 */
bool ProfileRegistry::has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return profiles_.count(name) > 0;
}

/**
 * @brief List all registered profile names.
 * @return Sorted vector of profile names.
 * @internal
 * @version 1.9.7
 */
std::vector<std::string> ProfileRegistry::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(profiles_.size());
    for (const auto& [k, _] : profiles_) {
        names.push_back(k);
    }
    std::sort(names.begin(), names.end());
    return names;
}

/**
 * @brief Number of registered profiles.
 * @return Count of profiles.
 * @internal
 * @version 1.9.7
 */
size_t ProfileRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return profiles_.size();
}

} // namespace entropic
