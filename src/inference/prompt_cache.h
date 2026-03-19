/**
 * @file prompt_cache.h
 * @brief Host-memory KV cache state storage with LRU eviction.
 *
 * Caches processed system prompt prefixes (identity + constitution + tools)
 * in host RAM. On tier swap, restores cached prefix via
 * llama_state_seq_set_data instead of re-processing prompt tokens.
 *
 * Internal to librentropic-inference. Does not cross .so boundaries.
 * Thread-safe: all public methods acquire the internal mutex.
 *
 * @version 1.8.3
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief 64-bit hash used as cache lookup key.
 *
 * Hash of (prompt_text + '\0' + model_path). Not a security hash --
 * this is a cache key where speed matters and collision across <20
 * entries is negligible.
 *
 * @version 1.8.3
 */
struct CacheKey {
    uint64_t hash;  ///< Combined hash value

    bool operator==(const CacheKey& other) const { return hash == other.hash; }
};

/**
 * @brief Hash function for CacheKey in unordered containers.
 * @version 1.8.3
 */
struct CacheKeyHash {
    /**
     * @brief Hash operator for CacheKey.
     * @param key Cache key to hash.
     * @return Hash value (identity — already hashed).
     * @version 1.8.3
     */
    size_t operator()(const CacheKey& key) const { return key.hash; }
};

/**
 * @brief Single cached KV state snapshot.
 * @version 1.8.3
 */
struct CacheEntry {
    std::vector<uint8_t> data;   ///< Raw KV cache bytes
    int token_count;             ///< Prompt tokens covered by this entry
    size_t data_size;            ///< data.size() for quick byte accounting
};

/**
 * @brief Cumulative cache performance counters.
 * @version 1.8.3
 */
struct CacheStats {
    uint64_t hits = 0;           ///< Successful lookups
    uint64_t misses = 0;         ///< Failed lookups
    uint64_t evictions = 0;      ///< LRU evictions
    uint64_t stores = 0;         ///< Successful stores
    size_t peak_bytes = 0;       ///< High-water mark of bytes_used
};

/**
 * @brief Host-memory KV cache with LRU eviction.
 *
 * Stores KV cache snapshots keyed by content hash of the system prompt
 * text concatenated with the model path. Evicts least-recently-used
 * entries when the configured RAM limit is exceeded.
 *
 * @par Thread safety
 * All public methods acquire mutex_. The expensive llama.cpp calls
 * (llama_decode, llama_state_seq_get/set_data) happen OUTSIDE the
 * cache mutex in the caller (LlamaCppBackend).
 *
 * @par Lifecycle
 * @code
 *   PromptCache cache(max_bytes);
 *   cache.store(key, data, token_count);   // after system prompt decode
 *   auto* entry = cache.lookup(key);       // before next decode
 *   cache.clear();                         // on model unload
 * @endcode
 *
 * @version 1.8.3
 */
class PromptCache {
public:
    /**
     * @brief Construct with maximum RAM budget.
     * @param max_bytes Maximum total bytes across all cached entries.
     *        0 = caching disabled.
     * @version 1.8.3
     */
    explicit PromptCache(size_t max_bytes);

    /**
     * @brief Store a KV cache snapshot.
     * @param key Hash of prompt content + model path.
     * @param data Raw KV cache bytes from llama_state_seq_get_data.
     * @param token_count Number of prompt tokens this entry covers.
     * @return true if stored (may evict older entries), false if entry
     *         exceeds max_bytes entirely and cannot be stored.
     * @version 1.8.3
     */
    bool store(const CacheKey& key,
               std::vector<uint8_t>&& data,
               int token_count);

    /**
     * @brief Retrieve a cached KV snapshot.
     * @param key Hash to look up.
     * @return Pointer to cached entry if found, nullptr on miss.
     *         Pointer valid until next store() or clear() call.
     *         Updates LRU ordering on hit.
     * @version 1.8.3
     */
    const CacheEntry* lookup(const CacheKey& key);

    /**
     * @brief Evict all entries. Called on model reload.
     * @version 1.8.3
     */
    void clear();

    /**
     * @brief Current total bytes consumed by cached entries.
     * @return Byte count.
     * @version 1.8.3
     */
    size_t bytes_used() const;

    /**
     * @brief Number of cached entries.
     * @return Entry count.
     * @version 1.8.3
     */
    size_t entry_count() const;

    /**
     * @brief Cache hit/miss statistics.
     * @return Copy of current stats.
     * @version 1.8.3
     */
    CacheStats stats() const;

    /**
     * @brief Compute a cache key from prompt text and model path.
     * @param prompt_text Full system prompt string.
     * @param model_path Model file path string.
     * @return CacheKey with combined hash.
     * @version 1.8.3
     */
    static CacheKey make_key(std::string_view prompt_text,
                             std::string_view model_path);

private:
    /**
     * @brief Evict LRU entries until at least needed_bytes are free.
     * @param needed_bytes Space to reclaim.
     * @version 1.8.3
     */
    void evict_until(size_t needed_bytes);

    size_t max_bytes_;     ///< Maximum total cached bytes
    size_t bytes_used_;    ///< Current total cached bytes
    CacheStats stats_;     ///< Performance counters

    /// Map from cache key to stored entry.
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> entries_;

    /// LRU list: front = most recently used, back = eviction candidate.
    std::list<CacheKey> lru_;

    /// Reverse lookup: key → position in LRU list.
    std::unordered_map<CacheKey, std::list<CacheKey>::iterator,
                       CacheKeyHash> lru_map_;

    mutable std::mutex mutex_;  ///< Guards all mutable state
};

} // namespace entropic
