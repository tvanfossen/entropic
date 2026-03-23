/**
 * @file context_manager.cpp
 * @brief Context management implementation.
 * @version 1.8.4
 */

#include <entropic/core/context_manager.h>
#include <entropic/types/logging.h>

static auto logger = entropic::log::get("core.context_manager");

namespace entropic {

/**
 * @brief Construct a context manager.
 * @param compaction Compaction manager reference.
 * @param callbacks Engine callbacks reference.
 * @param hooks Engine-level hooks.
 * @internal
 * @version 1.8.4
 */
ContextManager::ContextManager(
    CompactionManager& compaction,
    EngineCallbacks& callbacks,
    ContextManagerHooks hooks)
    : compaction_(compaction),
      callbacks_(callbacks),
      hooks_(std::move(hooks)) {}

/**
 * @brief Refresh context limit from tier config.
 * @param ctx Loop context (unused, kept for interface consistency).
 * @param context_length New max tokens (0 = no change).
 * @internal
 * @version 1.8.4
 */
void ContextManager::refresh_context_limit(
    LoopContext& ctx,
    int context_length) {
    (void)ctx;
    if (context_length <= 0) {
        return;
    }
    if (context_length != compaction_.counter.max_tokens) {
        logger->debug("Updating context limit: {} -> {}",
                      compaction_.counter.max_tokens, context_length);
        compaction_.counter.max_tokens = context_length;
    }
}

/**
 * @brief Replace old tool results with stubs.
 * @param ctx Loop context.
 * @param keep_recent Number of recent results to keep.
 * @return (pruned_count, freed_chars).
 * @internal
 * @version 1.8.4
 */
std::pair<int, int> ContextManager::prune_tool_results(
    LoopContext& ctx,
    int keep_recent) {
    std::vector<size_t> indices;
    for (size_t i = 0; i < ctx.messages.size(); ++i) {
        auto it = ctx.messages[i].metadata.find("tool_name");
        if (it != ctx.messages[i].metadata.end()) {
            indices.push_back(i);
        }
    }

    size_t cut = 0;
    if (keep_recent > 0
        && indices.size() > static_cast<size_t>(keep_recent)) {
        cut = indices.size() - static_cast<size_t>(keep_recent);
    } else {
        return {0, 0};
    }

    int pruned = 0;
    int freed = 0;
    for (size_t j = 0; j < cut; ++j) {
        auto& msg = ctx.messages[indices[j]];
        if (msg.content.rfind("[Previous:", 0) == 0) {
            continue;
        }
        auto name_it = msg.metadata.find("tool_name");
        std::string name = (name_it != msg.metadata.end())
                             ? name_it->second : "unknown";
        int chars = static_cast<int>(msg.content.size());
        freed += chars;
        msg.content = "[Previous: " + name + " result — "
                    + std::to_string(chars) + " chars, pruned]";
        ++pruned;
    }

    if (pruned > 0) {
        compaction_.counter.clear_cache();
    }
    return {pruned, freed};
}

/**
 * @brief Auto-prune tool results older than TTL iterations.
 * @param ctx Loop context.
 * @internal
 * @version 1.8.4
 */
void ContextManager::prune_old_tool_results(LoopContext& ctx) {
    int ttl = compaction_.config.tool_result_ttl;
    int current = ctx.metrics.iterations;
    int pruned = 0;

    for (auto& msg : ctx.messages) {
        auto tn = msg.metadata.find("tool_name");
        if (tn == msg.metadata.end()) {
            continue;
        }
        if (msg.content.rfind("[Previous:", 0) == 0) {
            continue;
        }
        auto ai = msg.metadata.find("added_at_iteration");
        if (ai == msg.metadata.end()) {
            continue;
        }
        int added = 0;
        try { added = std::stoi(ai->second); }
        catch (...) { continue; }
        if (current - added < ttl) {
            continue;
        }
        int chars = static_cast<int>(msg.content.size());
        msg.content = "[Previous: " + tn->second + " result — "
                    + std::to_string(chars) + " chars, pruned]";
        ++pruned;
    }

    if (pruned > 0) {
        compaction_.counter.clear_cache();
        logger->info("[AUTO-PRUNE] Pruned {} results (TTL={})", pruned, ttl);
    }
}

/**
 * @brief Inject context usage warning if over threshold.
 * @param ctx Loop context.
 * @internal
 * @version 1.8.4
 */
void ContextManager::inject_context_warning(LoopContext& ctx) {
    float threshold = compaction_.config.warning_threshold_percent;
    float usage = compaction_.counter.usage_percent(ctx.messages);
    if (usage < threshold) {
        return;
    }

    auto last = ctx.metadata.find("last_warning_iteration");
    std::string iter_str = std::to_string(ctx.metrics.iterations);
    if (last != ctx.metadata.end() && last->second == iter_str) {
        return;
    }

    int max_tok = compaction_.counter.max_tokens;
    int cur_tok = compaction_.counter.count_messages(ctx.messages);
    int pct = static_cast<int>(usage * 100.0f);

    Message warning;
    warning.role = "user";
    warning.content = "[CONTEXT WARNING] Context at "
        + std::to_string(pct) + "% capacity ("
        + std::to_string(cur_tok) + "/" + std::to_string(max_tok)
        + " tokens). Capture findings with entropic.todo if needed,"
          " then call entropic.prune_context.";
    ctx.messages.push_back(std::move(warning));
    ctx.metadata["last_warning_iteration"] = iter_str;
    logger->info("[WARNING] Context at {}% — warning injected", pct);
}

/**
 * @brief Check and perform compaction if needed.
 * @param ctx Loop context.
 * @param force Bypass threshold check.
 * @internal
 * @version 1.8.8
 */
void ContextManager::check_compaction(
    LoopContext& ctx,
    bool force) {
    // Pass conversation_id so compaction can snapshot (v1.8.8)
    auto result = compaction_.check_and_compact(
        ctx.messages, force, ctx.conversation_id);

    if (result.compacted) {
        logger->info("Compacted: {} -> {} tokens",
                     result.old_token_count, result.new_token_count);
        if (callbacks_.on_compaction != nullptr) {
            // Format minimal JSON for callback
            std::string json = "{\"old\":" + std::to_string(result.old_token_count)
                + ",\"new\":" + std::to_string(result.new_token_count) + "}";
            callbacks_.on_compaction(json.c_str(), callbacks_.user_data);
        }
        if (hooks_.after_compaction) {
            hooks_.after_compaction(ctx);
        }
    }
}

} // namespace entropic
