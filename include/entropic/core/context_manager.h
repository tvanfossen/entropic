/**
 * @file context_manager.h
 * @brief Context management subsystem for the agentic loop.
 *
 * Handles context limit refresh, tool result pruning, context warning
 * injection, and compaction delegation.
 *
 * @version 1.8.4
 */

#pragma once

#include <entropic/core/compaction.h>
#include <entropic/core/engine_types.h>
#include <entropic/interfaces/i_hook_handler.h>

#include <functional>

namespace entropic {

/**
 * @brief Engine-level hooks called during context management.
 * @version 1.8.4
 */
struct ContextManagerHooks {
    std::function<void(LoopContext&)> after_compaction; ///< Post-compaction hook
};

/**
 * @brief Handles context management for the agentic loop.
 *
 * Subsystem of AgentEngine. Manages context limit refresh, tool result
 * pruning, context warning injection, and compaction.
 *
 * @version 1.8.4
 */
class ContextManager {
public:
    /**
     * @brief Construct a context manager.
     * @param compaction Compaction manager (shared reference).
     * @param callbacks Engine callbacks (shared reference).
     * @param hooks Engine-level hooks.
     * @version 1.8.4
     */
    ContextManager(CompactionManager& compaction,
                   EngineCallbacks& callbacks,
                   ContextManagerHooks hooks = {});

    /**
     * @brief Refresh context limit based on tier config.
     * @param ctx Loop context.
     * @param context_length New context length (0 = no change).
     * @version 1.8.4
     */
    void refresh_context_limit(LoopContext& ctx, int context_length);

    /**
     * @brief Replace old tool results with stubs.
     * @param ctx Loop context.
     * @param keep_recent Number of recent results to keep.
     * @return (pruned_count, freed_chars).
     * @version 1.8.4
     */
    std::pair<int, int> prune_tool_results(
        LoopContext& ctx,
        int keep_recent);

    /**
     * @brief Auto-prune tool results older than TTL iterations.
     * @param ctx Loop context.
     * @version 1.8.4
     */
    void prune_old_tool_results(LoopContext& ctx);

    /**
     * @brief Inject context usage warning if over threshold.
     * @param ctx Loop context.
     * @version 1.8.4
     */
    void inject_context_warning(LoopContext& ctx);

    /**
     * @brief Check and perform compaction if needed.
     * @param ctx Loop context.
     * @param force Bypass threshold check.
     * @version 1.8.4
     */
    void check_compaction(LoopContext& ctx, bool force = false);

    /**
     * @brief Set the hook dispatch interface.
     * @param hooks Hook dispatch interface.
     * @utility
     * @version 1.9.1
     */
    void set_hooks(const HookInterface& hooks) { hook_iface_ = hooks; }

private:
    CompactionManager& compaction_; ///< Compaction manager
    EngineCallbacks& callbacks_;    ///< Shared callbacks
    ContextManagerHooks hooks_;     ///< Engine hooks
    HookInterface hook_iface_;      ///< Hook dispatch (v1.9.1)
};

} // namespace entropic
