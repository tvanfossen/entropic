// SPDX-License-Identifier: Apache-2.0
/**
 * @file conversation_state.h
 * @brief gh#144: one caller's conversation, as a value.
 *
 * @par Provenance
 * Adapted from `src/facade/conversation_state.h` (v2.0.1), which was written
 * for a facade-owned conversation, included by nothing, and never reached
 * production. Its four operations mapped one-to-one onto the engine's
 * `conversation_` clusters, so the shape was right and the owner was wrong.
 * The old file is deleted rather than left as a dead duplicate.
 *
 * `append_user` / `append_result` are deliberately NOT carried over: their
 * return-a-copy contract does not match `run_drain_loop`, which appends in
 * place. Keeping them would have invited a second, subtly different append
 * path beside the real one.
 *
 * @version 2.12.0
 */

#pragma once

#include <entropic/types/message.h>

#include <cstdint>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief One caller-scoped conversation and its KV bookkeeping.
 *
 * The engine holds a map of these keyed by an opaque, caller-supplied
 * `session_key`. `""` is the default session and behaves exactly as the
 * single shared conversation did before v2.12.0.
 *
 * @utility
 * @version 2.12.0
 */
struct ConversationState {
    /// @brief Full message history, including the seeded system prompt.
    std::vector<Message> messages;

    /// @brief Epoch seconds of the last turn on this session, for LRU KV
    ///        eviction. History itself is never evicted — it is
    ///        authoritative and re-prefillable.
    int64_t last_activity_epoch_s = 0;

    /**
     * @brief Drop the history (a new session under the same key).
     * @utility
     * @version 2.12.0
     */
    void clear() { messages.clear(); }

    /**
     * @brief Number of messages held.
     * @return Message count.
     * @utility
     * @version 2.12.0
     */
    std::size_t count() const { return messages.size(); }
};

}  // namespace entropic
