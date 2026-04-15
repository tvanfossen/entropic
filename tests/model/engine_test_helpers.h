// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file engine_test_helpers.h
 * @brief Shared helpers for engine-loop model tests.
 *
 * CallbackState, mock executors, mock tier resolution, callback
 * wiring. Used by individual engine test executables (E0-E8).
 *
 * @version 1.10.2
 */

#pragma once

#include "model_test_context.h"

#include <entropic/core/engine.h>
#include <entropic/core/engine_types.h>

// ── Shared callback state ───────────────────────────────────

/// @brief State recording for engine callbacks.
/// @internal
/// @version 1.10.2
struct CallbackState {
    std::vector<int> states;         ///< AgentState values observed
    std::string tier;                ///< Tier selected by routing
    bool compaction_fired = false;   ///< on_compaction called
    bool delegation_started = false; ///< on_delegation_start called
    int tool_exec_count = 0;        ///< Tool executor invocation count
};

// ── Callback functions ──────────────────────────────────────

/**
 * @brief Record state transitions.
 * @param state AgentState as int.
 * @param ud CallbackState pointer.
 * @callback
 * @version 1.10.2
 */
inline void cb_state_change(int state, void* ud) {
    static_cast<CallbackState*>(ud)->states.push_back(state);
}

/**
 * @brief Record tier selection.
 * @param tier Tier name.
 * @param ud CallbackState pointer.
 * @callback
 * @version 1.10.2
 */
inline void cb_tier_selected(const char* tier, void* ud) {
    static_cast<CallbackState*>(ud)->tier = tier;
}

/**
 * @brief Record compaction event.
 * @param json Compaction JSON.
 * @param ud CallbackState pointer.
 * @callback
 * @version 1.10.2
 */
inline void cb_compaction(const char* /*json*/, void* ud) {
    static_cast<CallbackState*>(ud)->compaction_fired = true;
}

/**
 * @brief Record delegation start.
 * @param child_id Child conversation ID.
 * @param tier Target tier.
 * @param task Task description.
 * @param ud CallbackState pointer.
 * @callback
 * @version 1.10.2
 */
inline void cb_delegation_start(const char* /*child_id*/,
                                const char* /*tier*/,
                                const char* /*task*/,
                                void* ud) {
    static_cast<CallbackState*>(ud)->delegation_started = true;
}

/**
 * @brief Wire callbacks to a CallbackState.
 * @param cbs Output EngineCallbacks.
 * @param state CallbackState to record into.
 * @utility
 * @version 1.10.2
 */
inline void wire_callbacks(EngineCallbacks& cbs, CallbackState& state) {
    cbs.on_state_change = cb_state_change;
    cbs.on_tier_selected = cb_tier_selected;
    cbs.on_compaction = cb_compaction;
    cbs.on_delegation_start = cb_delegation_start;
    cbs.user_data = &state;
}

// ── Mock tool executor ──────────────────────────────────────

/**
 * @brief Mock tool executor that returns scripted file content.
 * @param ctx Loop context.
 * @param tool_calls Tool calls to process.
 * @param user_data CallbackState pointer.
 * @return Tool result messages.
 * @callback
 * @version 1.10.2
 */
inline std::vector<Message> mock_tool_exec(
    LoopContext& /*ctx*/,
    const std::vector<ToolCall>& tool_calls,
    void* user_data) {
    auto* state = static_cast<CallbackState*>(user_data);
    state->tool_exec_count++;
    std::vector<Message> results;
    for (const auto& tc : tool_calls) {
        Message msg;
        msg.role = "tool";
        msg.content = "File content: hello world\n(tool: " + tc.id + ")";
        results.push_back(std::move(msg));
    }
    return results;
}

/**
 * @brief Mock tool executor that denies write and returns error.
 * @param ctx Loop context.
 * @param tool_calls Tool calls to process.
 * @param user_data CallbackState pointer.
 * @return Denial message.
 * @callback
 * @version 1.10.2
 */
inline std::vector<Message> auth_deny_tool_exec(
    LoopContext& /*ctx*/,
    const std::vector<ToolCall>& tool_calls,
    void* user_data) {
    auto* state = static_cast<CallbackState*>(user_data);
    state->tool_exec_count++;
    std::vector<Message> results;
    for (const auto& tc : tool_calls) {
        Message msg;
        msg.role = "tool";
        msg.content = "DENIED: identity 'reader' lacks WRITE access for "
                      + tc.id + ". Only READ permitted.";
        results.push_back(std::move(msg));
    }
    return results;
}

// ── Mock tier resolution ────────────────────────────────────

/**
 * @brief Mock tier resolution for delegation.
 * @param tier_name Target tier.
 * @param user_data Unused.
 * @return ChildContextInfo.
 * @callback
 * @version 1.10.2
 */
inline ChildContextInfo mock_resolve_tier(
    const std::string& tier_name, void* /*user_data*/) {
    ChildContextInfo info;
    info.valid = true;
    info.system_prompt = "You are " + tier_name + ".";
    return info;
}

/**
 * @brief Mock tier_exists check.
 * @param tier_name Tier to check.
 * @param user_data Unused.
 * @return true always.
 * @callback
 * @version 1.10.2
 */
inline bool mock_tier_exists(
    const std::string& /*tier_name*/, void* /*user_data*/) {
    return true;
}
