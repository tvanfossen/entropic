// SPDX-License-Identifier: Apache-2.0
/**
 * @file session_residency.h
 * @brief gh#144: which llama sequence holds which session's KV, purely.
 *
 * @par Why this exists
 * Warm-keep decides reuse by prefix-matching against ONE `resident_tokens_`
 * vector, hardcoded to sequence 0. With a single shared conversation that is
 * correct: the history is one monotonically growing sequence, so the previous
 * turn is always a prefix of the next.
 *
 * Keying conversations per caller breaks that assumption in the worst
 * direction. Two sessions on one handle share a system prompt, so
 * `common_prefix_len` is greater than zero, so `warm_keep_cut` returns a
 * NON-ZERO cut and `try_warm_reuse` takes the REUSE branch — `seq_rm`
 * destroying the other session's tail and, because it then returns true,
 * skipping the prompt cache as well. Worse than the pre-gh#96 path. The
 * in-repo comment claiming that case "falls back" was wrong; neither stated
 * branch fires (corrected in the same release).
 *
 * So residency must become per-sequence BEFORE keying ships. This header owns
 * the mapping and the per-slot token vectors; the backend owns the llama
 * calls. Pure and vendor-free so every rule is CPU-unit-testable — the same
 * reason `warm_keep_util.h` and `session_pool_util.h` are.
 *
 * @par Eviction
 * LRU on last use, and evicting drops KV ONLY — never message history, which
 * is authoritative and re-prefillable. An evicted session's next turn costs a
 * cold prefill, which is exactly what every turn costs today.
 *
 * @version 2.12.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/// @brief Sentinel for "no slot assigned".
constexpr int kNoSessionSlot = -1;

/**
 * @brief Per-session llama sequence slots and their resident tokens.
 *
 * @tparam Tok Token type (llama_token in production; int in tests).
 * @version 2.12.0
 */
template <typename Tok>
class SessionResidency {
public:
    /**
     * @brief Size the pool.
     * @param slots Number of llama sequences available (>= 1).
     * @version 2.12.0
     */
    explicit SessionResidency(int slots = 1)
        : slots_(slots < 1 ? 1 : slots), resident_(
              static_cast<std::size_t>(slots < 1 ? 1 : slots)) {}

    /**
     * @brief Slot currently holding this session, if any.
     * @param key Session key ("" is the default session).
     * @return Slot index, or kNoSessionSlot when not resident.
     * @req REQ-INFER-019
     * @version 2.12.0
     */
    int slot_for(const std::string& key) const {
        auto it = assigned_.find(key);
        return it == assigned_.end() ? kNoSessionSlot : it->second;
    }

    /**
     * @brief Assign a slot to this session, evicting the least-recently
     *        used one when the pool is full.
     *
     * @param key Session key.
     * @param[out] evicted Receives the key evicted to make room, empty when
     *             nothing was evicted. The caller must clear that slot's KV.
     * @return The slot now owned by `key`.
     * @req REQ-INFER-019
     * @version 2.12.0
     */
    int acquire(const std::string& key, std::string* evicted) {
        if (evicted != nullptr) { evicted->clear(); }
        auto it = assigned_.find(key);
        if (it != assigned_.end()) {
            touch(key);
            return it->second;
        }
        int slot = free_slot();
        if (slot == kNoSessionSlot) {
            const std::string victim = lru_key();
            slot = assigned_[victim];
            forget(victim);
            if (evicted != nullptr) { *evicted = victim; }
        }
        assigned_[key] = slot;
        touch(key);
        return slot;
    }

    /**
     * @brief Tokens recorded as resident in a slot.
     * @param slot Slot index.
     * @return The slot's resident tokens; empty for an invalid slot.
     * @req REQ-INFER-019
     * @version 2.12.0
     */
    const std::vector<Tok>& resident(int slot) const {
        static const std::vector<Tok> kEmpty;
        return valid(slot) ? resident_[static_cast<std::size_t>(slot)]
                           : kEmpty;
    }

    /**
     * @brief Record what a slot now holds after a decode.
     * @param slot Slot index.
     * @param tokens Tokens now resident.
     * @req REQ-INFER-019
     * @version 2.12.0
     */
    void set_resident(int slot, std::vector<Tok> tokens) {
        if (valid(slot)) {
            resident_[static_cast<std::size_t>(slot)] = std::move(tokens);
        }
    }

    /**
     * @brief Forget a slot's residency (its KV was cleared or invalidated).
     * @param slot Slot index.
     * @req REQ-INFER-019
     * @version 2.12.0
     */
    void invalidate(int slot) {
        if (valid(slot)) {
            resident_[static_cast<std::size_t>(slot)].clear();
        }
    }

    /**
     * @brief Forget every slot's residency.
     *
     * For the unconditional whole-context clears that still exist on some
     * paths — after one of those, no slot holds what we think it does.
     * @req REQ-INFER-019
     * @version 2.12.0
     */
    void invalidate_all() {
        for (auto& v : resident_) { v.clear(); }
    }

    /**
     * @brief Drop one SESSION's residency and release its slot (gh#165).
     *
     * `invalidate(slot)` needs a slot; a restore only knows a key, and after
     * a restore the session should not hold a slot at all — it has no
     * resident prefix worth keeping and another session may want the
     * sequence.
     *
     * Warm-keep is prefix-correct on its own (it reuses only the common
     * token prefix and seq_rm's the divergent tail), so this is belt AND
     * braces — deliberately. The reuse gate is token equality, not
     * conversation identity, so a future change that coarsened it would
     * silently decode a restored session against the prefix of the one it
     * replaced. Dropping the residency makes the restore correct by
     * construction rather than by an argument about the gate.
     *
     * @param key Session key.
     * @return The slot that was released, or kNoSessionSlot when the key
     *         held none. The caller must clear that slot's KV cells.
     * @req REQ-INFER-019
     * @req REQ-LOOP-010
     * @version 2.13.0
     */
    int forget_session(const std::string& key) {
        auto it = assigned_.find(key);
        if (it == assigned_.end()) { return kNoSessionSlot; }
        const int slot = it->second;
        invalidate(slot);
        forget(key);
        return slot;
    }

    /**
     * @brief Number of sequence slots in the pool.
     * @return Slot count (always >= 1).
     * @utility
     * @version 2.12.0
     */
    int slots() const { return slots_; }

    /**
     * @brief How many sessions currently hold a slot.
     * @return Assigned session count.
     * @utility
     * @version 2.12.0
     */
    std::size_t assigned_count() const { return assigned_.size(); }

private:
    /**
     * @brief Whether a slot index addresses this pool.
     * @param slot Slot index.
     * @return true when in range.
     * @utility
     * @version 2.12.0
     */
    bool valid(int slot) const {
        return slot >= 0 && slot < slots_;
    }

    /**
     * @brief Mark a session as most recently used.
     * @param key Session key.
     * @utility
     * @version 2.12.0
     */
    void touch(const std::string& key) { use_order_[key] = ++tick_; }

    /**
     * @brief Drop a session's slot assignment and LRU entry.
     * @param key Session key.
     * @utility
     * @version 2.12.0
     */
    void forget(const std::string& key) {
        assigned_.erase(key);
        use_order_.erase(key);
    }

    /**
     * @brief First slot no session currently holds.
     * @return Slot index, or kNoSessionSlot when the pool is full.
     * @utility
     * @version 2.12.0
     */
    int free_slot() const {
        std::vector<bool> taken(static_cast<std::size_t>(slots_), false);
        for (const auto& [k, s] : assigned_) {
            if (s >= 0 && s < slots_) {
                taken[static_cast<std::size_t>(s)] = true;
            }
        }
        int found = kNoSessionSlot;
        for (int i = 0; i < slots_ && found == kNoSessionSlot; ++i) {
            if (!taken[static_cast<std::size_t>(i)]) { found = i; }
        }
        return found;
    }

    /**
     * @brief Session key with the oldest use tick.
     * @return The eviction victim.
     * @utility
     * @version 2.12.0
     */
    std::string lru_key() const {
        std::string oldest;
        uint64_t best = UINT64_MAX;
        for (const auto& [k, t] : use_order_) {
            if (t < best) { best = t; oldest = k; }
        }
        return oldest;
    }

    int slots_;
    std::vector<std::vector<Tok>> resident_;
    std::unordered_map<std::string, int> assigned_;
    std::unordered_map<std::string, uint64_t> use_order_;
    uint64_t tick_ = 0;
};

}  // namespace entropic
