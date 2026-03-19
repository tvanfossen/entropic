/**
 * @file prompt_cache.cpp
 * @brief PromptCache implementation — LRU eviction, thread-safe.
 *
 * Uses FNV-1a 64-bit for cache key hashing. Not a security hash —
 * cache key over <20 entries where collision worst-case is a cache
 * miss, not a security breach.
 *
 * @version 1.8.3
 */

#include "prompt_cache.h"

#include <entropic/types/logging.h>

#include <algorithm>

namespace entropic {

namespace {

auto logger = entropic::log::get("inference.prompt_cache");

/**
 * @brief FNV-1a 64-bit hash.
 * @param data Input byte range.
 * @param len Byte count.
 * @return 64-bit hash.
 * @utility
 * @version 1.8.3
 */
uint64_t fnv1a_64(const char* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(static_cast<uint8_t>(data[i]));
        h *= 1099511628211ULL;
    }
    return h;
}

} // anonymous namespace

/**
 * @brief Construct with maximum RAM budget.
 * @param max_bytes Maximum total bytes. 0 disables caching.
 * @version 1.8.3
 */
PromptCache::PromptCache(size_t max_bytes)
    : max_bytes_(max_bytes)
    , bytes_used_(0)
{
}

/**
 * @brief Compute cache key from prompt text and model path.
 *
 * Concatenates prompt_text + '\0' + model_path and hashes with FNV-1a.
 * The null separator prevents prefix collisions.
 *
 * @param prompt_text Full system prompt string.
 * @param model_path Model file path string.
 * @return CacheKey with combined hash.
 * @internal
 * @version 1.8.3
 */
CacheKey PromptCache::make_key(std::string_view prompt_text,
                               std::string_view model_path)
{
    std::string combined;
    combined.reserve(prompt_text.size() + 1 + model_path.size());
    combined.append(prompt_text);
    combined.push_back('\0');
    combined.append(model_path);
    return CacheKey{fnv1a_64(combined.data(), combined.size())};
}

/**
 * @brief Store a KV cache snapshot.
 *
 * If the entry size exceeds max_bytes, returns false without storing.
 * Otherwise evicts LRU entries as needed and stores the new entry.
 *
 * @param key Hash of prompt content + model path.
 * @param data Raw KV cache bytes (moved).
 * @param token_count Prompt tokens covered.
 * @return true if stored, false if too large.
 * @internal
 * @version 1.8.3
 */
bool PromptCache::store(const CacheKey& key,
                        std::vector<uint8_t>&& data,
                        int token_count)
{
    std::lock_guard<std::mutex> lock(mutex_);

    size_t entry_size = data.size();

    if (max_bytes_ == 0) {
        return false;
    }

    if (entry_size > max_bytes_) {
        logger->warn("Cache entry ({} bytes) exceeds max_bytes ({}), skipping",
                     entry_size, max_bytes_);
        return false;
    }

    // Remove existing entry with same key if present
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        bytes_used_ -= it->second.data_size;
        entries_.erase(it);
        auto lru_it = lru_map_.find(key);
        if (lru_it != lru_map_.end()) {
            lru_.erase(lru_it->second);
            lru_map_.erase(lru_it);
        }
    }

    evict_until(entry_size);

    CacheEntry entry;
    entry.data = std::move(data);
    entry.token_count = token_count;
    entry.data_size = entry_size;

    entries_[key] = std::move(entry);
    lru_.push_front(key);
    lru_map_[key] = lru_.begin();

    bytes_used_ += entry_size;
    ++stats_.stores;
    if (bytes_used_ > stats_.peak_bytes) {
        stats_.peak_bytes = bytes_used_;
    }

    logger->info("Cached prompt prefix: {} bytes, {} tokens, {} entries total",
                 entry_size, token_count, entries_.size());
    return true;
}

/**
 * @brief Look up a cached KV snapshot.
 *
 * On hit, moves the entry to front of LRU list and increments hit
 * counter. On miss, increments miss counter.
 *
 * @param key Hash to look up.
 * @return Pointer to entry on hit, nullptr on miss.
 * @internal
 * @version 1.8.3
 */
const CacheEntry* PromptCache::lookup(const CacheKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(key);
    if (it == entries_.end()) {
        ++stats_.misses;
        return nullptr;
    }

    // Move to front of LRU
    auto lru_it = lru_map_.find(key);
    if (lru_it != lru_map_.end()) {
        lru_.splice(lru_.begin(), lru_, lru_it->second);
    }

    ++stats_.hits;
    return &it->second;
}

/**
 * @brief Evict all entries.
 * @internal
 * @version 1.8.3
 */
void PromptCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    lru_.clear();
    lru_map_.clear();
    bytes_used_ = 0;
    logger->info("Prompt cache cleared");
}

/**
 * @brief Current total bytes consumed.
 * @return Byte count.
 * @internal
 * @version 1.8.3
 */
size_t PromptCache::bytes_used() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_used_;
}

/**
 * @brief Number of cached entries.
 * @return Entry count.
 * @internal
 * @version 1.8.3
 */
size_t PromptCache::entry_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

/**
 * @brief Cache performance statistics.
 * @return Copy of current stats.
 * @internal
 * @version 1.8.3
 */
CacheStats PromptCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

/**
 * @brief Evict LRU entries until needed_bytes can fit.
 *
 * Pops from the back of the LRU list (least recently used)
 * until bytes_used_ + needed_bytes <= max_bytes_.
 *
 * @param needed_bytes Space required for the incoming entry.
 * @internal
 * @version 1.8.3
 */
void PromptCache::evict_until(size_t needed_bytes) {
    while (bytes_used_ + needed_bytes > max_bytes_ && !lru_.empty()) {
        CacheKey victim = lru_.back();
        lru_.pop_back();
        lru_map_.erase(victim);

        auto it = entries_.find(victim);
        if (it != entries_.end()) {
            bytes_used_ -= it->second.data_size;
            ++stats_.evictions;
            logger->info("Evicted cache entry: {} bytes freed",
                         it->second.data_size);
            entries_.erase(it);
        }
    }
}

} // namespace entropic
