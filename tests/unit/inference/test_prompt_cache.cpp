/**
 * @file test_prompt_cache.cpp
 * @brief Unit tests for PromptCache — cache lifecycle, LRU eviction,
 *        key computation, and thread safety.
 *
 * Tests operate on PromptCache in isolation (no model, no GPU).
 *
 * @version 1.8.3
 */

#include <catch2/catch_test_macros.hpp>

#include "prompt_cache.h"

#include <thread>
#include <vector>

using entropic::CacheKey;
using entropic::PromptCache;

namespace {

/**
 * @brief Create a test data buffer of specified size.
 * @param size Buffer size in bytes.
 * @param fill Fill byte value.
 * @return Vector of fill bytes.
 * @version 1.8.3
 */
std::vector<uint8_t> make_data(size_t size, uint8_t fill = 0xAB) {
    return std::vector<uint8_t>(size, fill);
}

} // anonymous namespace

// ── Store and lookup ──────────────────────────────────────

SCENARIO("Store and lookup", "[prompt_cache]") {
    GIVEN("an empty cache with 1KB limit") {
        PromptCache cache(1024);

        WHEN("a 100B entry is stored") {
            CacheKey key = PromptCache::make_key("sys prompt", "/model.gguf");
            bool ok = cache.store(key, make_data(100), 50);

            THEN("it is retrievable") {
                REQUIRE(ok);
                auto* entry = cache.lookup(key);
                REQUIRE(entry != nullptr);
                REQUIRE(entry->data_size == 100);
                REQUIRE(entry->token_count == 50);
                REQUIRE(cache.bytes_used() == 100);
                REQUIRE(cache.entry_count() == 1);
            }
        }
    }
}

// ── Lookup miss ───────────────────────────────────────────

SCENARIO("Lookup miss", "[prompt_cache]") {
    GIVEN("an empty cache") {
        PromptCache cache(1024);
        CacheKey key = PromptCache::make_key("nonexistent", "/model.gguf");

        WHEN("lookup is called") {
            auto* entry = cache.lookup(key);

            THEN("nullptr is returned and miss counter increments") {
                REQUIRE(entry == nullptr);
                REQUIRE(cache.stats().misses == 1);
            }
        }
    }
}

// ── Content-addressed keys ────────────────────────────────

SCENARIO("Content-addressed keys", "[prompt_cache]") {
    GIVEN("two entries with different prompt text") {
        PromptCache cache(4096);
        CacheKey key_a = PromptCache::make_key("prompt A", "/model.gguf");
        CacheKey key_b = PromptCache::make_key("prompt B", "/model.gguf");

        WHEN("both are stored") {
            cache.store(key_a, make_data(100, 0xAA), 10);
            cache.store(key_b, make_data(200, 0xBB), 20);

            THEN("each is retrievable by its own key") {
                auto* a = cache.lookup(key_a);
                auto* b = cache.lookup(key_b);
                REQUIRE(a != nullptr);
                REQUIRE(b != nullptr);
                REQUIRE(a->data_size == 100);
                REQUIRE(b->data_size == 200);
                REQUIRE(a->data[0] == 0xAA);
                REQUIRE(b->data[0] == 0xBB);
            }
        }
    }
}

// ── Same prompt, different model ──────────────────────────

SCENARIO("Same prompt different model", "[prompt_cache]") {
    GIVEN("two entries with identical prompt but different models") {
        PromptCache cache(4096);
        CacheKey key_a = PromptCache::make_key("same prompt", "/model_a.gguf");
        CacheKey key_b = PromptCache::make_key("same prompt", "/model_b.gguf");

        WHEN("both are stored") {
            cache.store(key_a, make_data(100), 10);
            cache.store(key_b, make_data(200), 20);

            THEN("they produce different keys and coexist") {
                REQUIRE(key_a.hash != key_b.hash);
                REQUIRE(cache.entry_count() == 2);
                auto* a = cache.lookup(key_a);
                auto* b = cache.lookup(key_b);
                REQUIRE(a != nullptr);
                REQUIRE(b != nullptr);
                REQUIRE(a->data_size == 100);
                REQUIRE(b->data_size == 200);
            }
        }
    }
}

// ── LRU eviction order ────────────────────────────────────

SCENARIO("LRU eviction order", "[prompt_cache]") {
    GIVEN("a cache with max_bytes=100") {
        PromptCache cache(100);
        CacheKey key_a = PromptCache::make_key("a", "/m");
        CacheKey key_b = PromptCache::make_key("b", "/m");
        CacheKey key_c = PromptCache::make_key("c", "/m");

        WHEN("A(40B), B(40B), then C(40B) are stored") {
            cache.store(key_a, make_data(40), 5);
            cache.store(key_b, make_data(40), 5);
            cache.store(key_c, make_data(40), 5);

            THEN("A is evicted (LRU) and B,C remain") {
                REQUIRE(cache.lookup(key_a) == nullptr);
                REQUIRE(cache.lookup(key_b) != nullptr);
                REQUIRE(cache.lookup(key_c) != nullptr);
                REQUIRE(cache.entry_count() == 2);
                REQUIRE(cache.bytes_used() == 80);
            }
        }
    }
}

// ── Lookup updates LRU ────────────────────────────────────

SCENARIO("Lookup updates LRU", "[prompt_cache]") {
    GIVEN("entries A,B in a full cache (max_bytes=80)") {
        PromptCache cache(80);
        CacheKey key_a = PromptCache::make_key("a", "/m");
        CacheKey key_b = PromptCache::make_key("b", "/m");
        cache.store(key_a, make_data(40), 5);
        cache.store(key_b, make_data(40), 5);

        WHEN("A is looked up then C is stored") {
            cache.lookup(key_a);  // refreshes A
            CacheKey key_c = PromptCache::make_key("c", "/m");
            cache.store(key_c, make_data(40), 5);

            THEN("B is evicted (A was refreshed by lookup)") {
                REQUIRE(cache.lookup(key_a) != nullptr);
                REQUIRE(cache.lookup(key_b) == nullptr);
                REQUIRE(cache.lookup(key_c) != nullptr);
            }
        }
    }
}

// ── Entry exceeds limit ───────────────────────────────────

SCENARIO("Entry exceeds limit", "[prompt_cache]") {
    GIVEN("max_bytes=100") {
        PromptCache cache(100);

        WHEN("a 200B entry is stored") {
            CacheKey key = PromptCache::make_key("big", "/m");
            bool ok = cache.store(key, make_data(200), 10);

            THEN("store returns false and cache is empty") {
                REQUIRE_FALSE(ok);
                REQUIRE(cache.entry_count() == 0);
                REQUIRE(cache.bytes_used() == 0);
            }
        }
    }
}

// ── Clear evicts all ──────────────────────────────────────

SCENARIO("Clear evicts all", "[prompt_cache]") {
    GIVEN("a cache with 5 entries") {
        PromptCache cache(10000);
        for (int i = 0; i < 5; ++i) {
            CacheKey key = PromptCache::make_key(
                "prompt" + std::to_string(i), "/m");
            cache.store(key, make_data(100), 10);
        }
        REQUIRE(cache.entry_count() == 5);

        WHEN("clear() is called") {
            cache.clear();

            THEN("bytes_used=0, entry_count=0") {
                REQUIRE(cache.bytes_used() == 0);
                REQUIRE(cache.entry_count() == 0);
            }
        }
    }
}

// ── Stats tracking ────────────────────────────────────────

SCENARIO("Stats tracking", "[prompt_cache]") {
    GIVEN("a cache with max_bytes=100") {
        PromptCache cache(100);
        CacheKey key_a = PromptCache::make_key("a", "/m");
        CacheKey key_b = PromptCache::make_key("b", "/m");
        CacheKey key_c = PromptCache::make_key("c", "/m");

        WHEN("stores, hits, misses, and evictions occur") {
            cache.store(key_a, make_data(40), 5);   // store 1
            cache.store(key_b, make_data(40), 5);   // store 2
            cache.lookup(key_a);                      // hit 1
            cache.lookup(key_c);                      // miss 1
            cache.store(key_c, make_data(40), 5);   // store 3, evicts A

            THEN("stats reflect all operations") {
                auto s = cache.stats();
                REQUIRE(s.stores == 3);
                REQUIRE(s.hits == 1);
                REQUIRE(s.misses == 1);
                REQUIRE(s.evictions == 1);
                REQUIRE(s.peak_bytes == 80);
            }
        }
    }
}

// ── Zero max_bytes disables ───────────────────────────────

SCENARIO("Zero max_bytes disables caching", "[prompt_cache]") {
    GIVEN("max_bytes=0") {
        PromptCache cache(0);

        WHEN("store() is called") {
            CacheKey key = PromptCache::make_key("test", "/m");
            bool ok = cache.store(key, make_data(10), 5);

            THEN("returns false and nothing stored") {
                REQUIRE_FALSE(ok);
                REQUIRE(cache.entry_count() == 0);
            }
        }
    }
}

// ── Bytes accounting ──────────────────────────────────────

SCENARIO("Bytes accounting", "[prompt_cache]") {
    GIVEN("a cache tracking bytes") {
        PromptCache cache(500);
        CacheKey key_a = PromptCache::make_key("a", "/m");
        CacheKey key_b = PromptCache::make_key("b", "/m");

        WHEN("entries are stored and evicted") {
            cache.store(key_a, make_data(100), 5);
            REQUIRE(cache.bytes_used() == 100);

            cache.store(key_b, make_data(200), 10);
            REQUIRE(cache.bytes_used() == 300);

            cache.clear();
            REQUIRE(cache.bytes_used() == 0);

            THEN("bytes_used matches expected sum at every step") {
                // Verified inline above
                REQUIRE(cache.entry_count() == 0);
            }
        }
    }
}

// ── Concurrent access ─────────────────────────────────────

SCENARIO("Concurrent access", "[prompt_cache]") {
    GIVEN("4 threads performing interleaved operations") {
        PromptCache cache(100000);

        WHEN("threads store, lookup, and clear concurrently") {
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&cache, t]() {
                    for (int i = 0; i < 50; ++i) {
                        std::string id = std::to_string(t * 50 + i);
                        CacheKey key = PromptCache::make_key(id, "/m");
                        cache.store(key, make_data(100), 5);
                        cache.lookup(key);
                        if (i % 25 == 0) {
                            cache.clear();
                        }
                    }
                });
            }
            for (auto& th : threads) {
                th.join();
            }

            THEN("no crash or corruption (TSAN would flag races)") {
                REQUIRE(cache.bytes_used() <= 100000);
            }
        }
    }
}

// ── Duplicate key overwrites ──────────────────────────────

SCENARIO("Duplicate key overwrites existing entry", "[prompt_cache]") {
    GIVEN("a cache with an existing entry") {
        PromptCache cache(1024);
        CacheKey key = PromptCache::make_key("prompt", "/model.gguf");
        cache.store(key, make_data(100, 0xAA), 10);

        WHEN("same key is stored with different data") {
            cache.store(key, make_data(200, 0xBB), 20);

            THEN("entry is updated, bytes accounting correct") {
                REQUIRE(cache.entry_count() == 1);
                REQUIRE(cache.bytes_used() == 200);
                auto* entry = cache.lookup(key);
                REQUIRE(entry != nullptr);
                REQUIRE(entry->data_size == 200);
                REQUIRE(entry->token_count == 20);
                REQUIRE(entry->data[0] == 0xBB);
            }
        }
    }
}
