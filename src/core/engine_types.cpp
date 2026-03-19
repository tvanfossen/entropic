/**
 * @file engine_types.cpp
 * @brief Implementation of engine type utilities.
 * @version 1.8.4
 */

#include <entropic/core/engine_types.h>

namespace entropic {

/**
 * @brief Get loop duration in milliseconds.
 * @return Duration in ms (end_time - start_time) * 1000.
 * @utility
 * @version 1.8.4
 */
int LoopMetrics::duration_ms() const {
    return static_cast<int>((end_time - start_time) * 1000);
}

/// @brief Lookup table for AgentState names indexed by enum value.
/// @internal
static const char* const kStateNames[] = {
    "IDLE", "PLANNING", "EXECUTING", "WAITING_TOOL", "VERIFYING",
    "DELEGATING", "COMPLETE", "ERROR", "INTERRUPTED", "PAUSED",
};

/**
 * @brief Get the string name for an AgentState value.
 * @param state Agent state.
 * @return Static string like "IDLE", "EXECUTING", etc.
 * @utility
 * @version 1.8.4
 */
const char* agent_state_name(AgentState state) {
    auto idx = static_cast<int>(state);
    if (idx >= 0 && idx < static_cast<int>(sizeof(kStateNames) / sizeof(kStateNames[0]))) {
        return kStateNames[idx];
    }
    return "UNKNOWN";
}

} // namespace entropic
